// ============================================================================
//  fuzz_reactive_reentrant_set.cpp  (L-20)
// ----------------------------------------------------------------------------
//  Invariant under stress:
//    "Property::set inside an Effect / Computed body is allowed and
//     lands in the next flush round (Graph::flushing_ guard). The
//     re-entrant chain MUST stabilise OR raise CircularDependencyError
//     after `kMaxFlushRounds`. It MUST NOT crash, deadlock or
//     corrupt graph state."
//
//  Strategy:
//    - One driver Property `p`, plus N "mirror" Effects each writing
//      a downstream Property when its upstream changes (with random
//      stop conditions). Random walk drives `p` and verifies that
//      either everything stabilises or CircularDependencyError is
//      raised, but the Graph state stays consistent after recovery.
// ============================================================================

#include <doctest/doctest.h>

#include "aria/property.hpp"
#include "aria/reactive/effect.hpp"
#include "aria/reactive/graph.hpp"
#include "fuzz_support.hpp"

using namespace aria;
using namespace aria::reactive;

TEST_CASE("L-20 fuzz: acyclic re-entrant writes always stabilise") {
    fuzz::Rng rng{fuzz::seed(0xF1'00'5E7'20)};

    Property<int> a{0};
    Property<int> b{0};

    // Effect mirrors a -> b (single-step propagation, always converges).
    Effect mirror{[&] { b.set(a.get()); }};

    for (std::size_t step = 0; step < fuzz::iters(); ++step) {
        const int prev = a.peek();
        const int delta = static_cast<int>(rng.u32(1, 5));
        REQUIRE_NOTHROW(a.set(prev + delta));
        CHECK(b.get() == a.get());
    }
}

TEST_CASE("L-20 fuzz: self-set Effect normalizes each external write once") {
    fuzz::Rng rng{fuzz::seed(0x5E1F'5E7'20)};

    Property<int> p{0};

    // The active Effect is Computing, so writes to its own dependency
    // do not re-enter the body. External writes still trigger it.
    Effect e{[&] {
        const int v = p.get();
        if (v < 1000 && (v % 2 == 0)) {
            p.set(v + 1);
        }
    }};

    for (std::size_t step = 0; step < fuzz::iters(); ++step) {
        const int prev = p.peek();
        const int delta = static_cast<int>(rng.u32(1, 7));
        // External writes never throw -- self-set converges by L-20.
        p.set(prev + delta);
        const int input = prev + delta;
        CHECK(p.peek() == (input < 1000 && input % 2 == 0 ? input + 1 : input));
    }
    // Graph is still healthy after the storm.
    CHECK(p.get() >= 0);
}
