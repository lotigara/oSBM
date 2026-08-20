
#ifdef __cplusplus

#include <new>
#include <rpmalloc.h>

#ifndef __CRTDECL
#define __CRTDECL
#endif

// oSBM: world-scope allocations are drawn from the world's own heap so its
// spans empty when it is destroyed. Null unless the calling thread is inside a
// world; see StarMemoryDomain.hpp. Declared by hand to keep this vendored
// header free of engine includes.
extern "C" void* starMemoryDomainActiveHeap(void);

static inline void* rpnew_domain_alloc(std::size_t size) {
#if RPMALLOC_FIRST_CLASS_HEAPS
	if (void* heap = starMemoryDomainActiveHeap())
		return rpmalloc_heap_alloc((rpmalloc_heap_t*)heap, size);
#endif
	return rpmalloc(size);
}

static inline void* rpnew_domain_aligned_alloc(std::size_t alignment, std::size_t size) {
#if RPMALLOC_FIRST_CLASS_HEAPS
	if (void* heap = starMemoryDomainActiveHeap())
		return rpmalloc_heap_aligned_alloc((rpmalloc_heap_t*)heap, alignment, size);
#endif
	return rpaligned_alloc(alignment, size);
}



extern void __CRTDECL
operator delete(void* p) noexcept {
	rpfree(p);
}

extern void __CRTDECL
operator delete[](void* p) noexcept {
	rpfree(p);
}

// oSBM: the throwing forms MUST throw std::bad_alloc on failure. Upstream
// returned rpmalloc()'s result directly, so an out-of-memory `new` handed back
// a null pointer and the caller hard-faulted on first use -- no exception, no
// std::terminate, no crash log. On Switch that turned a recoverable
// out-of-memory into a silent process death (observed: the game vanished with
// nothing written to crash.txt while the heap sat at 95% of its budget).
//
// Throwing instead lets the engine's existing handlers do their job: world
// creation already catches std::exception and fails the warp gracefully rather
// than taking the process down. Follows the standard retry-via-new_handler
// protocol so any installed handler gets its chance to free memory first.
static inline void* rpnew_allocate_or_throw(std::size_t size) {
	while (true) {
		if (void* p = rpnew_domain_alloc(size))
			return p;
		std::new_handler handler = std::get_new_handler();
		if (!handler)
			throw std::bad_alloc();
		handler();
	}
}

extern void* __CRTDECL
operator new(std::size_t size) noexcept(false) {
	return rpnew_allocate_or_throw(size);
}

extern void* __CRTDECL
operator new[](std::size_t size) noexcept(false) {
	return rpnew_allocate_or_throw(size);
}

extern void* __CRTDECL
operator new(std::size_t size, const std::nothrow_t& tag) noexcept {
	(void)sizeof(tag);
	return rpnew_domain_alloc(size);
}

extern void* __CRTDECL
operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
	(void)sizeof(tag);
	return rpnew_domain_alloc(size);
}

#if (__cplusplus >= 201402L || _MSC_VER >= 1916)

extern void __CRTDECL
operator delete(void* p, std::size_t size) noexcept {
	(void)sizeof(size);
	rpfree(p);
}

extern void __CRTDECL
operator delete[](void* p, std::size_t size) noexcept {
	(void)sizeof(size);
	rpfree(p);
}

#endif

#if (__cplusplus > 201402L || defined(__cpp_aligned_new))

extern void __CRTDECL
operator delete(void* p, std::align_val_t align) noexcept {
	(void)sizeof(align);
	rpfree(p);
}

extern void __CRTDECL
operator delete[](void* p, std::align_val_t align) noexcept {
	(void)sizeof(align);
	rpfree(p);
}

extern void __CRTDECL
operator delete(void* p, std::size_t size, std::align_val_t align) noexcept {
	(void)sizeof(size);
	(void)sizeof(align);
	rpfree(p);
}

extern void __CRTDECL
operator delete[](void* p, std::size_t size, std::align_val_t align) noexcept {
	(void)sizeof(size);
	(void)sizeof(align);
	rpfree(p);
}

extern void* __CRTDECL
operator new(std::size_t size, std::align_val_t align) noexcept(false) {
	return rpnew_domain_aligned_alloc(static_cast<size_t>(align), size);
}

extern void* __CRTDECL
operator new[](std::size_t size, std::align_val_t align) noexcept(false) {
	return rpnew_domain_aligned_alloc(static_cast<size_t>(align), size);
}

extern void* __CRTDECL
operator new(std::size_t size, std::align_val_t align, const std::nothrow_t& tag) noexcept {
	(void)sizeof(tag);
	return rpnew_domain_aligned_alloc(static_cast<size_t>(align), size);
}

extern void* __CRTDECL
operator new[](std::size_t size, std::align_val_t align, const std::nothrow_t& tag) noexcept {
	(void)sizeof(tag);
	return rpnew_domain_aligned_alloc(static_cast<size_t>(align), size);
}

#endif

#endif
