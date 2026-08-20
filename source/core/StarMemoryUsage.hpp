#pragma once

#include "StarString.hpp"

namespace Star {

// Process-wide memory accounting, used by the on-screen RAM indicator, the
// [perf-mem] log line and the low-memory reclaim path in Root.
//
// "used" and "budget" are whole-process numbers straight from the OS; the
// per-category counters below are engine bookkeeping that attributes the two
// caches which dominate mobile RSS (decompressed asset images, GPU texture
// atlases) so a player can see WHICH knob to turn, not just that they are
// close to the limit.
struct MemoryUsage {
  // Bytes the process currently holds. 0 when the platform cannot report it.
  uint64_t used = 0;
  // Bytes the process may hold before allocation fails or the OS kills it.
  // 0 when the platform exposes no meaningful ceiling.
  uint64_t budget = 0;

  bool valid() const;
  // 0.0f - 1.0f, or 0.0f when there is no budget to be a fraction of.
  float fraction() const;
};

// Queries the OS. Not free (a /proc read on Linux, a syscall elsewhere), so
// callers that poll should rate-limit; memoryUsageCached() does that for them.
MemoryUsage memoryUsage();

// Switch only, and the reason is worth stating: neither available figure works
// on its own there. svcGetInfo(UsedMemorySize) is constant-time but reports the
// heap libnx RESERVED up front -- always ~100% of the pool, so it can never
// signal pressure. newlib's mallinfo() reports genuinely live bytes but
// computes them by walking the whole free list, which grew to over a second per
// call late in a fragmented session. So the walk stays where it already
// happened before any of this existed -- the [perf-mem] line's fixed frame
// cadence -- and publishes its result here for memoryUsage() to hand out. A
// no-op on every other platform, which have a cheap accurate query.
void memoryUsageReportProcessBytes(uint64_t liveBytes);

// memoryUsage() with a ~500ms cache, safe to call every frame.
MemoryUsage memoryUsageCached();

enum class MemoryCategory : uint8_t {
  // Decompressed images/audio/json held by Assets' cache.
  AssetCache,
  // GPU texture atlases plus any lone textures the renderer allocated.
  TextureAtlas,
  // Root's shared Lua heap. Worth its own line because it is shared across
  // every world and script, so it is the one cache that a world teardown has
  // no obvious reason to shrink.
  Lua,
  Count
};

// Relaxed atomics; adding is a single fetch_add on a hot-ish path (texture
// upload, asset cache insert) so it must stay this cheap.
void memoryAccountAdd(MemoryCategory category, int64_t bytes);
// For owners that recompute their total periodically rather than tracking
// every insert and erase (Assets does this during its 5s cleanup sweep).
void memoryAccountSet(MemoryCategory category, uint64_t bytes);
uint64_t memoryAccountBytes(MemoryCategory category);

// "1.9G/3.1G 61%" style, for the RAM indicator and log lines.
String memoryUsageSummary();

// Asks the allocator to hand cached free memory back to the system, returning
// the bytes released (0 where the allocator has no such notion).
//
// This exists because freeing engine data is not the same as returning memory:
// rpmalloc keeps freed spans in per-thread and global caches. Measured on
// Switch hardware, dropping 157MB of cached assets moved the process figure by
// 1MB -- the rest stayed in those caches while world loads started failing for
// want of memory. Only meaningful on the rpmalloc builds (Switch, Android).
uint64_t memoryReleaseAllocatorCaches();

// Unmap empty spans cached on the calling thread only. Safe while other
// threads allocate; does not touch the global span cache.
uint64_t memoryReleaseThreadCaches();

// Bytes the allocator holds from the system heap. Anything the process has
// taken beyond this was allocated directly against libc -- on Switch that is
// the GL driver and libnx, which the engine's own accounting cannot see.
uint64_t memoryAllocatorMappedBytes();

}
