#include "rt/alloc_guard.hpp"
#include <atomic>
#include <cstdlib>
#include <new>

namespace
{
std::atomic<int> g_live { 0 };
std::atomic<std::size_t> g_count { 0 };
}

AllocGuard::AllocGuard() { if (g_live.fetch_add(1) == 0) g_count.store(0); }
AllocGuard::~AllocGuard() { g_live.fetch_sub(1); }
std::size_t AllocGuard::count() { return g_count.load(); }
bool AllocGuard::active() { return g_live.load() > 0; }

void* operator new(std::size_t n)
{
    if (g_live.load(std::memory_order_relaxed) > 0) g_count.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
