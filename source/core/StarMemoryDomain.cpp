#include "StarMemoryDomain.hpp"

#if defined(STAR_USE_RPMALLOC)
#include "rpmalloc.h"
#endif

namespace Star {

#if defined(STAR_USE_RPMALLOC) && RPMALLOC_FIRST_CLASS_HEAPS
// The heap allocations on this thread are drawn from right now. Null means the
// ordinary per-thread heap, which is what everything outside a world uses. Not
// in an anonymous namespace because the C hook below needs to name it.
thread_local void* t_currentHeap = nullptr;
#endif

void* memoryDomainAcquireHeap() {
  // Deliberately disabled. rpmalloc first-class heaps require ALL allocation
  // AND deallocation on a heap to be externally synchronized: a heap acquired
  // with rpmalloc_heap_acquire has owner_thread == 0, which makes
  // _rpmalloc_deallocate treat every thread as the owner and mutate the
  // heap's free lists directly instead of deferring. World allocations
  // legitimately escape their world (textures, packets, cached Json) and are
  // freed later from other threads -- after the world's heap has been
  // orphaned and re-acquired by ANOTHER world running on a different thread.
  // Those unsynchronized concurrent mutations corrupted the heap within
  // minutes on hardware (crashes in FlatHashTable iteration, newlib free,
  // garbage allocator counters). Per-thread heaps defer cross-thread frees
  // safely; span hoarding is handled by rpmalloc_release_caches maintenance
  // instead.
  return nullptr;
}

void memoryDomainReleaseHeap(void* heap) {
#if defined(STAR_USE_RPMALLOC) && RPMALLOC_FIRST_CLASS_HEAPS
  if (heap)
    rpmalloc_heap_release((rpmalloc_heap_t*)heap);
#else
  (void)heap;
#endif
}

MemoryDomain::MemoryDomain(void* heap) {
#if defined(STAR_USE_RPMALLOC) && RPMALLOC_FIRST_CLASS_HEAPS
  m_previous = t_currentHeap;
  t_currentHeap = heap;
#else
  (void)heap;
  m_previous = nullptr;
#endif
}

MemoryDomain::~MemoryDomain() {
#if defined(STAR_USE_RPMALLOC) && RPMALLOC_FIRST_CLASS_HEAPS
  t_currentHeap = m_previous;
#endif
}

}

extern "C" void* starMemoryDomainActiveHeap(void) {
#if defined(STAR_USE_RPMALLOC) && RPMALLOC_FIRST_CLASS_HEAPS
  return Star::t_currentHeap;
#else
  return nullptr;
#endif
}
