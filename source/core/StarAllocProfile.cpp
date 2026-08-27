#include "StarAllocProfile.hpp"

#ifdef STAR_ALLOC_PROFILE

#include <atomic>
#include <cstdio>
#include <cstring>

namespace Star {

// Depth rather than a bool: operator new implementations can nest (a handler
// that allocates), and a bool would clear the guard too early. Not in the
// anonymous namespace because the C hooks below need it.
thread_local int t_inNew = 0;

void allocProfileTrackFrom(void* pointer, size_t size, void* returnAddress, bool fromNew);

namespace {
  // One in this many allocations is recorded. Reported bytes are scaled back up
  // by the same factor, so the numbers are estimates whose accuracy improves
  // with the size of the leak -- which is exactly the case that matters.
  size_t const SampleRate = 64;

  // Allocations at or above this size are recorded EXACTLY rather than sampled.
  // Sampling estimates a population from 1/64 of it, which is fine for the
  // millions of small blocks but hopeless for the handful of big ones: 384 live
  // images of a few hundred KB are ~6 samples, so their contribution swings by
  // hundreds of MB. Big blocks are rare enough that tracking all of them costs
  // nothing, and they are exactly where a memory problem hides.
  size_t const ExactTrackSize = 64 * 1024;

  // Live sampled blocks. Sized so a session holding tens of millions of small
  // blocks still fits its 1/64 sample without the table saturating.
  size_t const PointerSlots = 1u << 17;
  size_t const SiteSlots = 4096;

  struct PointerSlot {
    std::atomic<uintptr_t> pointer{0};
    std::atomic<uint32_t> site{0};
    std::atomic<uint32_t> size{0};
    // 0 = raw malloc from C, 1 = C++ operator new. Recorded per block so the
    // split stays correct when the block is freed.
    std::atomic<uint32_t> fromNew{0};
    // 1 for exactly-tracked blocks, SampleRate for sampled ones. Bytes are
    // scaled at insert so the report needs no knowledge of how each was taken.
    std::atomic<uint32_t> weight{0};
  };

  struct SiteSlot {
    std::atomic<uintptr_t> returnAddress{0};
    std::atomic<int64_t> liveBytes{0};
    std::atomic<int64_t> liveCount{0};
    std::atomic<int64_t> totalCount{0};
  };

  // Plain BSS arrays, not calloc'd. This code now runs INSIDE rpmalloc, and
  // ::calloc is rpmalloc, so a lazy allocation here would re-enter the
  // allocator from the middle of an allocation. Zero-initialised atomics in
  // BSS need no constructor and are ready before main().
  PointerSlot g_pointerTable[PointerSlots];
  SiteSlot g_siteTable[SiteSlots];

  PointerSlot* pointerTable() {
    return g_pointerTable;
  }

  SiteSlot* siteTable() {
    return g_siteTable;
  }

  size_t hashPointer(uintptr_t p) {
    p >>= 4;
    p *= 0x9E3779B97F4A7C15ull;
    return (size_t)(p >> 32) & (PointerSlots - 1);
  }

  std::atomic<int64_t> g_cxxBytes{0};
  std::atomic<int64_t> g_mallocBytes{0};

  uint32_t siteFor(uintptr_t returnAddress) {
    SiteSlot* sites = siteTable();
    // Slot zero is reserved for overflow, so exhaustion cannot attribute all
    // later callers to whichever real allocation site happened to claim it.
    size_t h = (size_t)((returnAddress >> 2) * 2654435761u) % (SiteSlots - 1);
    for (size_t probe = 0; probe < 64; ++probe) {
      size_t idx = 1 + (h + probe) % (SiteSlots - 1);
      uintptr_t current = sites[idx].returnAddress.load(std::memory_order_relaxed);
      if (current == returnAddress)
        return (uint32_t)idx;
      if (current == 0) {
        uintptr_t expected = 0;
        if (sites[idx].returnAddress.compare_exchange_strong(expected, returnAddress))
          return (uint32_t)idx;
        if (expected == returnAddress)
          return (uint32_t)idx;
      }
    }
    return 0; // slot 0 doubles as the overflow bucket
  }
}

void allocProfileTrackFrom(void* pointer, size_t size, void* returnAddress, bool fromNew) {
  if (!pointer)
    return;

  // Thread-local counter rather than a shared one: a global atomic here would
  // serialise every allocation in the engine and change the timing being
  // measured.
  bool exact = size >= ExactTrackSize;
  static thread_local size_t counter = 0;
  if (!exact && (++counter % SampleRate != 0))
    return;
  uint32_t weight = exact ? 1u : (uint32_t)SampleRate;

  PointerSlot* table = pointerTable();
  if (!table)
    return;

  uint32_t site = siteFor((uintptr_t)returnAddress);
  size_t h = hashPointer((uintptr_t)pointer);
  for (size_t probe = 0; probe < 64; ++probe) {
    size_t idx = (h + probe) & (PointerSlots - 1);
    uintptr_t expected = 0;
    if (table[idx].pointer.compare_exchange_strong(expected, (uintptr_t)pointer)) {
      table[idx].site.store(site, std::memory_order_relaxed);
      table[idx].size.store((uint32_t)size, std::memory_order_relaxed);
      table[idx].fromNew.store(fromNew ? 1u : 0u, std::memory_order_relaxed);
      table[idx].weight.store(weight, std::memory_order_relaxed);
      int64_t scaled = (int64_t)size * (int64_t)weight;
      (fromNew ? g_cxxBytes : g_mallocBytes).fetch_add(scaled, std::memory_order_relaxed);
      SiteSlot* sites = siteTable();
      sites[site].liveBytes.fetch_add(scaled, std::memory_order_relaxed);
      sites[site].liveCount.fetch_add((int64_t)weight, std::memory_order_relaxed);
      sites[site].totalCount.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
  // Table full: drop the sample rather than evict, so live counts stay
  // conservative instead of drifting negative on the matching free.
}

void allocProfileUntrack(void* pointer) {
  if (!pointer)
    return;

  PointerSlot* table = pointerTable();
  if (!table)
    return;

  size_t h = hashPointer((uintptr_t)pointer);
  for (size_t probe = 0; probe < 64; ++probe) {
    size_t idx = (h + probe) & (PointerSlots - 1);
    uintptr_t current = table[idx].pointer.load(std::memory_order_relaxed);
    // Deliberately NO early-out on an empty slot. Untracking clears slots, so
    // empties appear in the middle of a probe chain; stopping at the first one
    // silently abandons entries stored past it, and their bytes then stay
    // "live" forever. That inflates exactly the high-churn sites the report
    // ranks first, which made an earlier hardware profile unusable.
    if (current != (uintptr_t)pointer)
      continue;

    uint32_t site = table[idx].site.load(std::memory_order_relaxed);
    uint32_t size = table[idx].size.load(std::memory_order_relaxed);
    uint32_t fromNew = table[idx].fromNew.load(std::memory_order_relaxed);
    uint32_t weight = table[idx].weight.load(std::memory_order_relaxed);
    if (table[idx].pointer.compare_exchange_strong(current, 0)) {
      int64_t scaled = (int64_t)size * (int64_t)weight;
      (fromNew ? g_cxxBytes : g_mallocBytes).fetch_add(-scaled, std::memory_order_relaxed);
      SiteSlot* sites = siteTable();
      sites[site].liveBytes.fetch_add(-scaled, std::memory_order_relaxed);
      sites[site].liveCount.fetch_add(-(int64_t)weight, std::memory_order_relaxed);
    }
    return;
  }
}

void allocProfileTrack(void* pointer, size_t size, void* returnAddress) {
  allocProfileTrackFrom(pointer, size, returnAddress, true);
}

int64_t allocProfileCxxBytes() {
  return g_cxxBytes.load(std::memory_order_relaxed);
}

int64_t allocProfileMallocBytes() {
  return g_mallocBytes.load(std::memory_order_relaxed);
}

void* allocProfileBaseAddress() {
  return (void*)&allocProfileBaseAddress;
}

void allocProfileReport(char* buffer, size_t bufferSize) {
  if (!buffer || !bufferSize)
    return;
  buffer[0] = 0;

  SiteSlot* sites = siteTable();
  if (!sites) {
    snprintf(buffer, bufferSize, " alloc profile unavailable");
    return;
  }

  uintptr_t base = (uintptr_t)allocProfileBaseAddress();
  int reported[8];
  size_t used = 0;
  for (int rank = 0; rank < 8; ++rank) {
    int64_t bestBytes = 0;
    int bestIdx = -1;
    for (size_t i = 0; i < SiteSlots; ++i) {
      bool alreadyReported = false;
      for (int r = 0; r < rank; ++r)
        alreadyReported |= (reported[r] == (int)i);
      if (alreadyReported)
        continue;
      int64_t bytes = sites[i].liveBytes.load(std::memory_order_relaxed);
      if (bytes > bestBytes) {
        bestBytes = bytes;
        bestIdx = (int)i;
      }
    }
    if (bestIdx < 0)
      break;
    reported[rank] = bestIdx;

    uintptr_t ra = sites[bestIdx].returnAddress.load(std::memory_order_relaxed);
    int64_t count = sites[bestIdx].liveCount.load(std::memory_order_relaxed);
    int written;
    if (bestIdx == 0)
      written = snprintf(buffer + used, bufferSize - used, " overflow=%lldkB/%lldn",
          (long long)(bestBytes >> 10), (long long)count);
    else
      written = snprintf(buffer + used, bufferSize - used, " +%llx=%lldkB/%lldn",
          (unsigned long long)(ra - base),
          (long long)(bestBytes >> 10),
          (long long)count);
    if (written <= 0 || (size_t)written >= bufferSize - used)
      break;
    used += (size_t)written;
  }
}
}

extern "C" {

void starAllocProfileEnterNew(void) {
  ++Star::t_inNew;
}

void starAllocProfileExitNew(void) {
  --Star::t_inNew;
}

void starAllocProfileTrackC(void* pointer, size_t size, void* returnAddress) {
  // Skip anything operator new is already bracketing: the C++ hook records it
  // with the engine caller's address, which is far more useful than a return
  // address inside the allocator.
  if (Star::t_inNew)
    return;
  Star::allocProfileTrackFrom(pointer, size, returnAddress, false);
}

void starAllocProfileUntrackC(void* pointer) {
  // Not guarded by t_inNew: a block allocated through either door can be freed
  // through this one, and untracking is keyed by pointer so it is correct
  // either way.
  Star::allocProfileUntrack(pointer);
}

}

#endif
