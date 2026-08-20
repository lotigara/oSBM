#include "StarJsonIntern.hpp"
#include "StarThread.hpp"
#include "StarXXHash.hpp"

#include <atomic>
#include <cstdlib>

namespace Star {

namespace {
  // Identity for an already-bottom-up-interned value. Children of a container
  // are canonical by the time it is offered, so comparing them by pointer is
  // both correct and O(1) -- structural recursion here would make interning a
  // whole asset tree quadratic.
  //
  // A value whose children did NOT come from the parser simply fails to match
  // an existing entry and gets inserted separately. That loses a dedup, which
  // is why this is only wired into the parse path.
  // Cheap 64-bit mixer. Interning runs on every parsed value during load, so
  // this is deliberately a few arithmetic ops rather than a streaming hash --
  // an XXHash64 per object entry measured as a 2.35x load-time regression.
  inline size_t mix(size_t h, size_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
  }

  struct InternHash {
    size_t operator()(Json const& v) const {
      size_t h = (size_t)v.type() * 0x100000001b3ull;
      switch (v.type()) {
        case Json::Type::String: {
          auto const& s = v.stringPtr()->utf8();
          XXHash64 hasher;
          hasher.push(s.data(), s.size());
          h = mix(h, (size_t)hasher.digest());
          break;
        }
        case Json::Type::Array: {
          for (auto const& e : *v.arrayPtr())
            h = mix(h, (size_t)internChildKey(e));
          break;
        }
        case Json::Type::Object: {
          // Iteration order differs between two equal objects built by
          // different insert orders, so entries are combined commutatively.
          size_t acc = 0;
          for (auto const& pair : *v.objectPtr()) {
            // The key's hash is already cheap to obtain; mixing it with the
            // canonical child pointer identifies the entry without touching
            // the child's contents at all.
            size_t entry = mix(Star::hash<String>()(pair.first), (size_t)internChildKey(pair.second));
            acc += entry;
          }
          h = mix(h, acc);
          break;
        }
        default:
          break;
      }
      return h;
    }

    // The identity of a child within its parent: canonical pointer for the
    // interned types, and for scalars a value the pointer space cannot collide
    // with is unnecessary -- scalars are compared directly by InternEquals, so
    // this only needs to be stable, not injective.
    static void const* internChildKey(Json const& v) {
      return v.payloadPointer();
    }
  };

  struct InternEquals {
    bool operator()(Json const& a, Json const& b) const {
      if (a.type() != b.type())
        return false;
      switch (a.type()) {
        case Json::Type::String:
          return a.payloadPointer() == b.payloadPointer() || *a.stringPtr() == *b.stringPtr();
        case Json::Type::Array: {
          auto const& la = *a.arrayPtr();
          auto const& lb = *b.arrayPtr();
          if (la.size() != lb.size())
            return false;
          for (size_t i = 0; i < la.size(); ++i) {
            if (!childEqual(la[i], lb[i]))
              return false;
          }
          return true;
        }
        case Json::Type::Object: {
          auto const& ma = *a.objectPtr();
          auto const& mb = *b.objectPtr();
          if (ma.size() != mb.size())
            return false;
          for (auto const& pair : ma) {
            auto other = mb.ptr(pair.first);
            if (!other || !childEqual(pair.second, *other))
              return false;
          }
          return true;
        }
        default:
          return false;
      }
    }

    // Canonical children compare by pointer. Scalars still compare by value so
    // that two objects differing only in a number are not merged.
    static bool childEqual(Json const& a, Json const& b) {
      if (a.type() != b.type())
        return false;
      switch (a.type()) {
        // Canonical children share one instance, so identity is the whole test
        // and costs a pointer compare with no reference-count traffic.
        case Json::Type::String:
        case Json::Type::Array:
        case Json::Type::Object: return a.payloadPointer() == b.payloadPointer();
        case Json::Type::Null:   return true;
        case Json::Type::Bool:   return a.toBool() == b.toBool();
        case Json::Type::Int:    return a.toInt() == b.toInt();
        case Json::Type::Float:  return a.toDouble() == b.toDouble();
        default: return false;
      }
    }
  };

  // The structural hash is computed once by the caller and carried with the
  // value: selecting a shard and probing the table would otherwise hash the
  // same value twice, which measured as roughly a quarter of the load cost.
  struct Entry {
    Json value;
    size_t hash;
  };

  struct EntryHash {
    size_t operator()(Entry const& e) const {
      return e.hash;
    }
  };

  struct EntryEquals {
    bool operator()(Entry const& a, Entry const& b) const {
      // Hash first: it rejects nearly every collision candidate without
      // touching the values. The structural check still runs, because a 64-bit
      // collision merging two different configs would be a silent data bug.
      return a.hash == b.hash && InternEquals()(a.value, b.value);
    }
  };

  // Sharded: asset loading parses on several worker threads at once, and a
  // single lock here would serialise them.
  size_t const ShardCount = 16;

  struct Shard {
    Mutex mutex;
    HashSet<Entry, EntryHash, EntryEquals> table;
  };

  Shard* shards() {
    static Shard s[ShardCount];
    return s;
  }

  std::atomic<uint64_t> s_hits{0};
  std::atomic<uint64_t> s_misses{0};

  bool internDefaultEnabled() {
#ifdef STAR_SYSTEM_FAMILY_MOBILE
    // STAR_JSON_INTERN=0 turns interning off for A/B soak tests.
    if (auto env = ::getenv("STAR_JSON_INTERN"))
      return env[0] != '0';
    return true;
#else
    // Off by default on desktop; STAR_JSON_INTERN=1 exercises the mobile
    // interning path in a desktop soak where debugging is cheap.
    if (auto env = ::getenv("STAR_JSON_INTERN"))
      return env[0] != '0';
    return false;
#endif
  }

  std::atomic<bool> s_enabled{internDefaultEnabled()};
}

bool jsonInternEnabled() {
  return s_enabled.load(std::memory_order_relaxed);
}

void jsonInternSetEnabled(bool enabled) {
  s_enabled.store(enabled, std::memory_order_relaxed);
  if (!enabled)
    jsonInternClear();
}

Json jsonIntern(Json v) {
  if (!s_enabled.load(std::memory_order_relaxed))
    return v;

  auto type = v.type();
  if (type != Json::Type::Object && type != Json::Type::Array && type != Json::Type::String)
    return v;

  Entry entry{std::move(v), 0};
  entry.hash = InternHash()(entry.value);
  // High bits for the shard: the low bits pick the bucket inside it.
  Shard& shard = shards()[(entry.hash >> 32) % ShardCount];

  MutexLocker locker(shard.mutex);
  auto existing = shard.table.find(entry);
  if (existing != shard.table.end()) {
    s_hits.fetch_add(1, std::memory_order_relaxed);
    return existing->value;
  }
  s_misses.fetch_add(1, std::memory_order_relaxed);
  shard.table.add(entry);
  return entry.value;
}

void jsonInternClear() {
  for (size_t i = 0; i < ShardCount; ++i) {
    // Same discipline as the asset cache: drop the entries outside the lock,
    // because freeing a whole table's worth of Json under it would stall every
    // thread still parsing.
    HashSet<Entry, EntryHash, EntryEquals> doomed;
    {
      MutexLocker locker(shards()[i].mutex);
      swap(doomed, shards()[i].table);
    }
  }
}

size_t jsonInternSize() {
  size_t total = 0;
  for (size_t i = 0; i < ShardCount; ++i) {
    MutexLocker locker(shards()[i].mutex);
    total += shards()[i].table.size();
  }
  return total;
}

uint64_t jsonInternHits() {
  return s_hits.load(std::memory_order_relaxed);
}

uint64_t jsonInternMisses() {
  return s_misses.load(std::memory_order_relaxed);
}

}
