#pragma once

#include "StarConfig.hpp"

// Per-world allocation heaps, so a world's memory can actually be returned when
// the world is destroyed.
//
// The problem, measured on hardware: rpmalloc only hands a 64KB span back when
// EVERY block in it is free. A world's millions of small allocations are
// interleaved with things that outlive it, so after returning to the ship the
// engine's live data is back to its baseline (214MB) while the allocator still
// holds 525MB of spans for it -- 40% occupancy, ~310MB free but unreturnable.
// In-world the same figure is 65-81%, so the damage is done by teardown, not by
// steady-state play.
//
// Giving each world its own heap means its allocations share spans only with
// each other. When the world dies its spans empty out and rpmalloc returns
// them. References that escape a world (an item that reached the player's
// inventory, an interned Json value) still pin their own span, but they no
// longer pin the whole world's worth.
//
// Two design points, both load-bearing:
//
//  * The heap belongs to the WORLD, not the thread. An earlier attempt used a
//    per-thread heap and measured as a complete null, because every world a
//    thread ever touched shared one heap -- the pinning moved rather than went
//    away.
//
//  * rpmalloc heaps are single-owner. This is safe because a world's heap is
//    only ever made current on that world's own thread: construction happens on
//    one worker before the world thread exists, and everything after runs on
//    WorldServerThread. Blocks may be FREED from any thread -- rpfree finds the
//    owning heap from the span and defers to it -- so only allocation is
//    constrained.
//
// Never calls rpmalloc_heap_free_all: world allocations legitimately escape,
// and freeing wholesale would be a use-after-free. Release returns the heap
// for reuse and unmaps its empty cached spans rather than pushing them into
// the global cache (which cannot be drained while other threads allocate).
namespace Star {

// Acquires a heap for a world. Returns null when first-class heaps are
// unavailable, in which case everything falls back to normal allocation.
void* memoryDomainAcquireHeap();

// Returns a world's heap once the world is gone. Does not free anything still
// live in it.
void memoryDomainReleaseHeap(void* heap);

// RAII: makes the given heap current for allocations on this thread. Nests, and
// restores the previous heap so a world tick inside another scope cannot leak
// its heap outward. A null heap is valid and means "use the normal allocator".
class MemoryDomain {
public:
  explicit MemoryDomain(void* heap);
  ~MemoryDomain();

  MemoryDomain(MemoryDomain const&) = delete;
  MemoryDomain& operator=(MemoryDomain const&) = delete;

private:
  void* m_previous;
};

}

// C linkage so the operator new implementations -- including the vendored
// rpnew.h, which must not include engine headers -- can reach it. Returns the
// current world heap, or null when this thread is not inside a world.
extern "C" void* starMemoryDomainActiveHeap(void);
