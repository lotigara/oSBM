#include "StarMemory.hpp"
// Declarations only; the profiler compiles to nothing unless STAR_ALLOC_PROFILE.
#include "StarAllocProfile.hpp"
#include "StarMemoryDomain.hpp"

#ifdef STAR_USE_JEMALLOC
#include "jemalloc/jemalloc.h"
#elif STAR_USE_MIMALLOC
#include "mimalloc.h"
#elif STAR_USE_RPMALLOC
// rpnew.h defines the global operator new/delete straight onto rpmalloc with no
// hook, so a profiling build must supply its own (below) instead of including
// it -- otherwise every engine allocation bypasses the profiler.
#ifndef STAR_ALLOC_PROFILE
#include "rpnew.h"
#else
#include "rpmalloc.h"
#endif

bool rpm_linker_ref() {
  rpmalloc_linker_reference();
  return true;
}

static bool _rpm_linker_ref = rpm_linker_ref();

#endif

namespace Star {

#ifdef STAR_USE_JEMALLOC
#ifdef STAR_JEMALLOC_IS_PREFIXED
  void* malloc(size_t size) {
    return je_malloc(size);
  }

  void* realloc(void* ptr, size_t size) {
    return je_realloc(ptr, size);
  }

  void free(void* ptr) {
    je_free(ptr);
  }

  void free(void* ptr, size_t size) {
    if (ptr)
      je_sdallocx(ptr, size, 0);
  }
#else
  void* malloc(size_t size) {
    return ::malloc(size);
  }

  void* realloc(void* ptr, size_t size) {
    return ::realloc(ptr, size);
  }

  void free(void* ptr) {
    ::free(ptr);
  }

  void free(void* ptr, size_t size) {
    ::free(ptr);
  }
#endif
#elif STAR_USE_MIMALLOC
  void* malloc(size_t size) {
  return mi_malloc(size);
  }

  void* realloc(void* ptr, size_t size) {
    return mi_realloc(ptr, size);
  }

  void free(void* ptr) {
    return mi_free(ptr);
  }

  void free(void* ptr, size_t size) {
    return mi_free_size(ptr, size);
  }
#elif STAR_USE_RPMALLOC
  void* malloc(size_t size) {
    return rpmalloc(size);
  }

  void* realloc(void* ptr, size_t size) {
    return rprealloc(ptr, size);
  }

  void free(void* ptr) {
    return rpfree(ptr);
  }

  void free(void* ptr, size_t) {
    return rpfree(ptr);
  }
#else
  void* malloc(size_t size) {
    return ::malloc(size);
  }

  void* realloc(void* ptr, size_t size) {
    return ::realloc(ptr, size);
  }

  void free(void* ptr) {
    return ::free(ptr);
  }

  void free(void* ptr, size_t) {
    return ::free(ptr);
  }
#endif

size_t spanFriendlyCapacity(size_t target, size_t minimum) {
#ifdef STAR_USE_RPMALLOC
  // Must match rpmalloc's _memory_span_size and SPAN_HEADER_SIZE. Getting these
  // wrong only costs the optimisation, never correctness -- the result is
  // always >= minimum.
  size_t const SpanSize = 64 * 1024;
  size_t const SpanHeader = 128;
  if (target < SpanSize)
    return target;
  size_t spans = (target + SpanHeader) / SpanSize;
  if (!spans)
    return target;
  size_t friendly = spans * SpanSize - SpanHeader;
  return friendly >= minimum ? friendly : target;
#else
  (void)minimum;
  return target;
#endif
}

}

#ifndef  STAR_USE_RPMALLOC

// The allocation profiler is hooked into this operator set as well as the
// rpmalloc one, so a desktop build can answer "what is holding the memory?"
// without a device in the loop. Same engine code, so the answer transfers --
// only the GL driver's own allocations are missing, and those are not visible
// to an operator-new hook anyway.
//
// The allocator hooks itself now, so operator new brackets its allocation with
// enter/exit: that suppresses the inner C hook and lets the block be recorded
// here instead, against the engine caller rather than a return address inside
// rpmalloc. Frees are left entirely to the allocator's hook, which sees every
// deallocation whichever door it came in through.
#ifdef STAR_ALLOC_PROFILE
#define STAR_PROFILE_ALLOC(size) \
  (starAllocProfileEnterNew(), Star::malloc(size))
#define STAR_PROFILE_TRACK(ptr, size) \
  (starAllocProfileExitNew(), Star::allocProfileTrack((ptr), (size), __builtin_return_address(0)))
// This operator set is the one used when rpmalloc is NOT in play (desktop
// jemalloc builds), so there is no allocator-level hook to untrack frees --
// it has to happen here. Leaving it a no-op made every allocation look live
// forever, which turned the desktop profile into a count of total allocations
// and made JSON parse temporaries look like the largest leak in the engine.
#define STAR_PROFILE_UNTRACK(ptr) Star::allocProfileUntrack(ptr)
#else
#define STAR_PROFILE_ALLOC(size) Star::malloc(size)
#define STAR_PROFILE_TRACK(ptr, size) ((void)0)
#define STAR_PROFILE_UNTRACK(ptr) ((void)0)
#endif

void* operator new(std::size_t size) {
  auto ptr = STAR_PROFILE_ALLOC(size);
  STAR_PROFILE_TRACK(ptr, size);
  if (!ptr)
    throw std::bad_alloc();
  return ptr;
}

void* operator new[](std::size_t size) {
  auto ptr = STAR_PROFILE_ALLOC(size);
  STAR_PROFILE_TRACK(ptr, size);
  if (!ptr)
    throw std::bad_alloc();
  return ptr;
}

// Globally override new and delete.  As the per standard, new and delete must
// be defined in global scope, and must not be inline.

void* operator new(std::size_t size, std::nothrow_t const&) noexcept {
  return Star::malloc(size);
}

void* operator new[](std::size_t size, std::nothrow_t const&) noexcept {
  return Star::malloc(size);
}

void operator delete(void* ptr) noexcept {
  STAR_PROFILE_UNTRACK(ptr);
  Star::free(ptr);
}

void operator delete[](void* ptr) noexcept {
  STAR_PROFILE_UNTRACK(ptr);
  Star::free(ptr);
}

void operator delete(void* ptr, std::nothrow_t const&) noexcept {
  STAR_PROFILE_UNTRACK(ptr);
  Star::free(ptr);
}

void operator delete[](void* ptr, std::nothrow_t const&) noexcept {
  STAR_PROFILE_UNTRACK(ptr);
  Star::free(ptr);
}

void operator delete(void* ptr, std::size_t size) noexcept {
  STAR_PROFILE_UNTRACK(ptr);
  Star::free(ptr, size);
}

void operator delete[](void* ptr, std::size_t size) noexcept {
  STAR_PROFILE_UNTRACK(ptr);
  Star::free(ptr, size);
}

#endif 
#if defined(STAR_USE_RPMALLOC) && defined(STAR_ALLOC_PROFILE)

// Profiling build only: same routing as rpnew.h, with the profiler hooked in.
// __builtin_return_address(0) here is the engine call site, because these ARE
// the operators the engine calls -- there is no thunk in between.
//
// Each allocation is bracketed so the allocator's own hook skips it. Without
// the bracket the block is recorded TWICE -- once by the hook inside rpmalloc,
// against a return address that lands in this very function, and once here
// against the real caller. An on-hardware run showed the consequence: 1134MB
// across 2,173,760 allocations piled onto a single "operator new" site that
// named nothing, while the same bytes were also counted in the C++ total.
static inline void* profiledAlloc(std::size_t size) {
  starAllocProfileEnterNew();
  // Must mirror rpnew.h's routing exactly. A profiling build does NOT include
  // rpnew.h, so hooking only one of the two silently measures the unsegregated
  // allocator while the shipping build uses the domain.
  void* ptr;
#if RPMALLOC_FIRST_CLASS_HEAPS
  if (void* heap = starMemoryDomainActiveHeap())
    ptr = rpmalloc_heap_alloc((rpmalloc_heap_t*)heap, size);
  else
    ptr = rpmalloc(size);
#else
  ptr = rpmalloc(size);
#endif
  starAllocProfileExitNew();
  return ptr;
}

void* operator new(std::size_t size) {
  void* ptr = profiledAlloc(size);
  if (!ptr)
    throw std::bad_alloc();
  Star::allocProfileTrack(ptr, size, __builtin_return_address(0));
  return ptr;
}

void* operator new[](std::size_t size) {
  void* ptr = profiledAlloc(size);
  if (!ptr)
    throw std::bad_alloc();
  Star::allocProfileTrack(ptr, size, __builtin_return_address(0));
  return ptr;
}

void* operator new(std::size_t size, std::nothrow_t const&) noexcept {
  void* ptr = profiledAlloc(size);
  Star::allocProfileTrack(ptr, size, __builtin_return_address(0));
  return ptr;
}

void* operator new[](std::size_t size, std::nothrow_t const&) noexcept {
  void* ptr = profiledAlloc(size);
  Star::allocProfileTrack(ptr, size, __builtin_return_address(0));
  return ptr;
}

void operator delete(void* ptr) noexcept {
  rpfree(ptr);
}

void operator delete[](void* ptr) noexcept {
  Star::allocProfileUntrack(ptr);
  rpfree(ptr);
}

void operator delete(void* ptr, std::nothrow_t const&) noexcept {
  Star::allocProfileUntrack(ptr);
  rpfree(ptr);
}

void operator delete[](void* ptr, std::nothrow_t const&) noexcept {
  Star::allocProfileUntrack(ptr);
  rpfree(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
  Star::allocProfileUntrack(ptr);
  rpfree(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept {
  Star::allocProfileUntrack(ptr);
  rpfree(ptr);
}

#endif