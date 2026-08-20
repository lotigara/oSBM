#pragma once

#include <new>

#include "StarConfig.hpp"

namespace Star {

// Don't want to override global C allocation functions, as our API is
// different.

void* malloc(size_t size);
void* realloc(void* ptr, size_t size);
void free(void* ptr);
void free(void* ptr, size_t size);

// Shrinks a *discretionary* growth target so a large block exactly fills the
// allocator's spans instead of spilling into another one.
//
// rpmalloc serves anything above its medium limit in whole 64KB spans and
// stores a 128 byte header inside the first, so a block of exactly 64KB needs
// 64KB+128 and is rounded up to TWO spans -- 100% waste. Geometric growth
// (capacity * 2) lands on precisely those powers of two, so the containers that
// dominate large allocations were routinely paying double. Measured on hardware:
// large/huge spans held ~546MB for ~200MB of live data.
//
// Only ever returns a value >= minimum, and only shrinks a target the caller
// chose for its own convenience, so no caller gets less than it asked for.
// Identity below the span size and on allocators without this behaviour.
size_t spanFriendlyCapacity(size_t target, size_t minimum);

}
