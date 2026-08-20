#pragma once

#include <algorithm>
#include <vector>

#include "StarConfig.hpp"

namespace Star {

// A map stored as a single sorted vector of pairs, looked up by binary search.
//
// Exists for Json objects. A hash map pays for empty slots and a stored hash:
// with power-of-two bucket counts and a 0.7 fill level, a four-entry
// StringMap<Json> holds nine 64-byte buckets -- 576 bytes to store 224 bytes of
// pairs. The same object here is exactly four pairs, and because Json values are
// immutable once parsed the vector can be shrunk to fit, so nothing is held in
// reserve either. Measured on hardware, those bucket arrays were the largest
// single live allocation site in the process.
//
// The trade is O(log n) lookup against O(1), and O(n) insertion in the middle.
// For the sizes this is used at -- objects with a handful of keys -- the binary
// search touches one or two cache lines where the hash map touches a random
// bucket, and building an object is dominated by parsing either way.
//
// Interface is the std::map subset that MapMixin and the engine actually use.
// Like Star's FlatHashMap, the stored pair has a non-const key: the container
// never exposes a way to reorder itself through it, and keeping it assignable is
// what allows the vector to shift elements on erase.
template <typename Key, typename Value, typename Compare = std::less<Key>>
class FlatSortedMap {
public:
  typedef Key key_type;
  typedef Value mapped_type;
  typedef pair<Key, Value> value_type;
  typedef std::vector<value_type> Storage;
  typedef typename Storage::iterator iterator;
  typedef typename Storage::const_iterator const_iterator;
  typedef size_t size_type;

  FlatSortedMap() = default;
  FlatSortedMap(FlatSortedMap const&) = default;
  FlatSortedMap(FlatSortedMap&&) = default;
  FlatSortedMap& operator=(FlatSortedMap const&) = default;
  FlatSortedMap& operator=(FlatSortedMap&&) = default;

  FlatSortedMap(std::initializer_list<value_type> list) {
    for (auto const& p : list)
      insert(p);
  }

  template <typename InputIterator>
  FlatSortedMap(InputIterator first, InputIterator last) {
    for (; first != last; ++first)
      insert(*first);
  }

  iterator begin() { return m_entries.begin(); }
  iterator end() { return m_entries.end(); }
  const_iterator begin() const { return m_entries.begin(); }
  const_iterator end() const { return m_entries.end(); }
  const_iterator cbegin() const { return m_entries.begin(); }
  const_iterator cend() const { return m_entries.end(); }

  size_type size() const { return m_entries.size(); }
  bool empty() const { return m_entries.empty(); }
  void clear() { m_entries.clear(); }
  void reserve(size_type n) { m_entries.reserve(n); }

  // Json objects are built once and then never modified, so handing back the
  // growth slack is free memory.
  void shrink_to_fit() { m_entries.shrink_to_fit(); }

  iterator find(Key const& k) {
    auto i = lowerBound(k);
    return (i != m_entries.end() && !keyLess(k, i->first)) ? i : m_entries.end();
  }

  const_iterator find(Key const& k) const {
    auto i = lowerBound(k);
    return (i != m_entries.end() && !keyLess(k, i->first)) ? i : m_entries.end();
  }

  size_type count(Key const& k) const { return find(k) == m_entries.end() ? 0 : 1; }

  pair<iterator, bool> insert(value_type v) {
    auto i = lowerBound(v.first);
    if (i != m_entries.end() && !keyLess(v.first, i->first))
      return {i, false};
    return {m_entries.insert(i, std::move(v)), true};
  }

  // Hinted insert. The hint is ignored -- the binary search is already cheap at
  // these sizes -- but the overload is needed by generic code that passes one.
  iterator insert(const_iterator, value_type v) {
    return insert(std::move(v)).first;
  }

  template <typename... Args>
  pair<iterator, bool> emplace(Args&&... args) {
    return insert(value_type(std::forward<Args>(args)...));
  }

  template <typename InputIterator>
  void insert(InputIterator first, InputIterator last) {
    for (; first != last; ++first)
      insert(*first);
  }

  Value& operator[](Key const& k) {
    auto i = lowerBound(k);
    if (i != m_entries.end() && !keyLess(k, i->first))
      return i->second;
    return m_entries.insert(i, value_type(k, Value()))->second;
  }

  Value& at(Key const& k) {
    auto i = find(k);
    if (i == m_entries.end())
      throw std::out_of_range("FlatSortedMap::at no such key");
    return i->second;
  }

  Value const& at(Key const& k) const {
    auto i = find(k);
    if (i == m_entries.end())
      throw std::out_of_range("FlatSortedMap::at no such key");
    return i->second;
  }

  size_type erase(Key const& k) {
    auto i = find(k);
    if (i == m_entries.end())
      return 0;
    m_entries.erase(i);
    return 1;
  }

  // Returns the following element, as std::map does. Erasing shifts everything
  // after this point, so callers must use the returned iterator rather than one
  // they held across the call.
  iterator erase(const_iterator pos) { return m_entries.erase(pos); }
  iterator erase(const_iterator first, const_iterator last) { return m_entries.erase(first, last); }

  void swap(FlatSortedMap& other) { m_entries.swap(other.m_entries); }

  bool operator==(FlatSortedMap const& other) const { return m_entries == other.m_entries; }
  bool operator!=(FlatSortedMap const& other) const { return !(*this == other); }

private:
  bool keyLess(Key const& a, Key const& b) const { return Compare()(a, b); }

  iterator lowerBound(Key const& k) {
    return std::lower_bound(m_entries.begin(), m_entries.end(), k,
        [](value_type const& e, Key const& key) { return Compare()(e.first, key); });
  }

  const_iterator lowerBound(Key const& k) const {
    return std::lower_bound(m_entries.begin(), m_entries.end(), k,
        [](value_type const& e, Key const& key) { return Compare()(e.first, key); });
  }

  Storage m_entries;
};

}
