#pragma once

#include "StarJson.hpp"

// Deduplication of parsed Json values ("hash consing").
//
// Mod-heavy asset sets repeat enormous amounts of identical JSON: the same
// stat blocks, icon specs, colour lists and whole sub-objects appear in
// hundreds of files. Json is immutable and implicitly shared, so structurally
// identical values can safely be collapsed onto a single instance and every
// holder simply shares the pointer.
//
// The parser interns bottom-up, so by the time a container is finished its
// children are already canonical. That makes identity cheap: two child
// containers are equal exactly when their pointers are equal, so a node hashes
// and compares in O(number of children) instead of walking its whole subtree.
// Values built outside the parser are simply never found in the table, which
// costs a missed dedup and never a wrong answer.
//
// The table holds a reference to everything it has seen, so it must not be
// left to grow forever -- see jsonInternClear, which callers run once loading
// has settled. Clearing keeps the savings: the duplicates are already sharing.
namespace Star {

// Interning trades load time for resident memory, so it is on by default only
// where memory is the binding constraint. Measured on a heavily modded asset
// set (FrackinUniverse), headless:
//
//   off : loads in  8.1s, 2264MB after fullyLoad
//   on  : loads in 17.7s, 2024MB after fullyLoad
//
// 240MB is 7.5% of the Switch's fixed 3189MB pool, which is the difference
// between finishing a session and dying of bad_alloc. A desktop has the memory
// to spare and would rather have the faster load, so it defaults off.
bool jsonInternEnabled();
void jsonInternSetEnabled(bool enabled);

// Returns the canonical instance for v, inserting it if this is the first
// sighting. Only Object, Array and String values are interned; scalars are
// cheaper to keep than to look up, and are returned unchanged.
//
// Interning must be bottom-up to be effective: canonical children are what
// make a container's identity a pointer comparison. Interning containers
// WITHOUT also interning strings measured *worse* than not interning at all
// (2313MB), because the table filled with containers that never matched.
Json jsonIntern(Json v);

// Drops every reference the table holds. Deduplicated values stay shared.
void jsonInternClear();

// Live entries, and how many lookups have hit an existing entry. Used by the
// engine's memory reporting to show whether interning is paying for itself.
size_t jsonInternSize();
uint64_t jsonInternHits();
uint64_t jsonInternMisses();

}
