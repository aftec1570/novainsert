// Test utility: counts global operator new allocations while any AllocGuard is alive. LIMITATION: malloc/calloc/realloc
// and aligned operator new are not intercepted (JUCE containers allocate via malloc) — see ledger S0 M2. Links its own operator new/delete into the
// test binary (tests/rt/alloc_guard.cpp). Never part of the product.
#pragma once
#include <cstddef>

struct AllocGuard
{
    AllocGuard();
    ~AllocGuard();
    static std::size_t count();   // allocations since the outermost live guard was created
    static bool active();         // true while at least one AllocGuard is alive
};

// Requires a live AllocGuard in scope: without one the counter is frozen and the check would pass vacuously.
#define REQUIRE_NO_ALLOC(expr)                                                     \
    do {                                                                           \
        REQUIRE(AllocGuard::active());                                             \
        const std::size_t before__ = AllocGuard::count();                          \
        (void) (expr);                                                             \
        REQUIRE(AllocGuard::count() == before__);                                  \
    } while (false)
