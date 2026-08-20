#pragma once

#include "StarConfig.hpp"

// Sampled live-allocation profiler, compiled in only when STAR_ALLOC_PROFILE is
// defined (a dedicated leak-hunt build; see the CMake option of the same name).
//
// Why this exists rather than the older newlib --wrap attribution in
// StarSwitchCompat.c: with rpmalloc enabled, operator new goes straight to
// rpmalloc, which takes its memory from newlib in big spans. The newlib
// wrappers therefore see the spans, not the engine allocations inside them, and
// attribute everything to rpmalloc. This hooks the layer the engine actually
// allocates at.
//
// It hooks BOTH doors: C++ operator new, and rpmalloc itself so that raw
// malloc from C code is attributed too. An earlier version watched only
// operator new and accounted for ~165MB of a 2.7GB process, because the GL
// driver, libnx, zlib, freetype and Lua all allocate as C.
//
// Every allocation is offered to the profiler; one in AllocProfileSampleRate is
// recorded with its caller, so reported live bytes are an estimate scaled back
// up by that rate. Frees probe the table, which is the one cost paid on a hot
// path -- acceptable in a build whose only purpose is finding a leak.
namespace Star {

void allocProfileTrack(void* pointer, size_t size, void* returnAddress);
void allocProfileUntrack(void* pointer);

// Live bytes split by which door the allocation came through: C++ operator new,
// or a raw malloc from C code. This split alone answers whether the memory
// belongs to the engine or to something underneath it.
int64_t allocProfileCxxBytes();
int64_t allocProfileMallocBytes();

// Top callers by estimated live bytes. Addresses are printed as offsets from
// allocProfileBaseAddress() so they survive the NRO's runtime load base and can
// be resolved offline with addr2line against the .elf.
void allocProfileReport(char* buffer, size_t bufferSize);
void* allocProfileBaseAddress();

}

// Hooks called from rpmalloc itself, so C allocations are attributed too.
// C linkage because rpmalloc.c is C.
extern "C" {

// Every allocation and free that passes through the allocator, whatever the
// language. Reentrancy-safe: no locks and no allocation, so it is callable from
// inside the allocator.
void starAllocProfileTrackC(void* pointer, size_t size, void* returnAddress);

void starAllocProfileUntrackC(void* pointer);

// operator new brackets its own call to the allocator with these so the C hook
// skips an allocation the C++ hook is about to attribute properly. Without it
// every C++ allocation would be recorded twice, and against rpmalloc's own code
// rather than the engine caller.
void starAllocProfileEnterNew(void);
void starAllocProfileExitNew(void);

}
