// Counting replacements of the global operator new / delete for this DLL (static CRT: they cover all of the menu's
// code and its libraries, and nothing of the game's). The size freed is the block's own (_msize), so the count stays
// right whichever delete form is used. Aligned new / delete keep the CRT's versions (and aren't counted).

#include "runtime/memory.h"

#include <atomic>
#include <cstdlib>
#include <malloc.h>
#include <new>

namespace
{
	std::atomic<std::int64_t> g_live{0};
	std::atomic<std::int64_t> g_count{0};

	void* Allocate(std::size_t n) noexcept
	{
		void* p = std::malloc(n ? n : 1);
		if (p) {
			g_live.fetch_add(static_cast<std::int64_t>(_msize(p)), std::memory_order_relaxed);
			g_count.fetch_add(1, std::memory_order_relaxed);
		}
		return p;
	}

	void Free(void* p) noexcept
	{
		if (!p) return;
		g_live.fetch_sub(static_cast<std::int64_t>(_msize(p)), std::memory_order_relaxed);
		std::free(p);
	}
}

void* operator new(std::size_t n)
{
	if (void* p = Allocate(n)) return p;
	throw std::bad_alloc();
}
void* operator new[](std::size_t n)
{
	if (void* p = Allocate(n)) return p;
	throw std::bad_alloc();
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return Allocate(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return Allocate(n); }
void operator delete(void* p) noexcept { Free(p); }
void operator delete[](void* p) noexcept { Free(p); }
void operator delete(void* p, std::size_t) noexcept { Free(p); }
void operator delete[](void* p, std::size_t) noexcept { Free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { Free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { Free(p); }

std::int64_t mirage::memory::LiveBytes() { return g_live.load(std::memory_order_relaxed); }
std::int64_t mirage::memory::TotalAllocations() { return g_count.load(std::memory_order_relaxed); }
