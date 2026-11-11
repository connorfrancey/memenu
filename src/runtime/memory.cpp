// Counting replacements of the global operator new / delete for this DLL (static CRT: they cover all of the menu's
// code and its libraries, and nothing of the game's), allocating with mimalloc. malloc / free stay the CRT's (mimalloc
// is built without MI_OVERRIDE), so memory from new must never be freed with free() or the other way round. The size
// counted is the block's own (mi_usable_size), so the count stays right whichever delete form is used.

#include "runtime/memory.h"

#include <atomic>
#include <cstdlib>
#include <new>

#include <mimalloc.h>

namespace
{
	std::atomic<std::int64_t> g_live{0};
	std::atomic<std::int64_t> g_count{0};

	void Count(void* p) noexcept
	{
		g_live.fetch_add(static_cast<std::int64_t>(mi_usable_size(p)), std::memory_order_relaxed);
		g_count.fetch_add(1, std::memory_order_relaxed);
	}

	void* Allocate(std::size_t n) noexcept
	{
		void* p = mi_malloc(n ? n : 1);
		if (p) Count(p);
		return p;
	}

	void* AllocateAligned(std::size_t n, std::align_val_t a) noexcept
	{
		void* p = mi_malloc_aligned(n ? n : 1, static_cast<std::size_t>(a));
		if (p) Count(p);
		return p;
	}

	void Free(void* p) noexcept
	{
		if (!p) return;
		g_live.fetch_sub(static_cast<std::int64_t>(mi_usable_size(p)), std::memory_order_relaxed);
		mi_free(p);
	}

	void* OrThrow(void* p)
	{
		if (!p) throw std::bad_alloc();
		return p;
	}
}

void* operator new(std::size_t n) { return OrThrow(Allocate(n)); }
void* operator new[](std::size_t n) { return OrThrow(Allocate(n)); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return Allocate(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return Allocate(n); }
void* operator new(std::size_t n, std::align_val_t a) { return OrThrow(AllocateAligned(n, a)); }
void* operator new[](std::size_t n, std::align_val_t a) { return OrThrow(AllocateAligned(n, a)); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return AllocateAligned(n, a); }
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return AllocateAligned(n, a); }

void operator delete(void* p) noexcept { Free(p); }
void operator delete[](void* p) noexcept { Free(p); }
void operator delete(void* p, std::size_t) noexcept { Free(p); }
void operator delete[](void* p, std::size_t) noexcept { Free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { Free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { Free(p); }
void operator delete(void* p, std::align_val_t) noexcept { Free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { Free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { Free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { Free(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { Free(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { Free(p); }

std::int64_t mirage::memory::LiveBytes() { return g_live.load(std::memory_order_relaxed); }
std::int64_t mirage::memory::TotalAllocations() { return g_count.load(std::memory_order_relaxed); }
