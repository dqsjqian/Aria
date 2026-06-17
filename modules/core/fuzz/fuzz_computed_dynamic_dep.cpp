// ============================================================================
//  fuzz_computed_dynamic_dep.cpp  (L-17)
// ----------------------------------------------------------------------------
//  Invariant under stress:
//    "AutoComputed / Effect rebuild their dependency edge set every
//     recompute. After branch flips, the old branch MUST NOT trigger
//     the computed any more (no ghost subscriptions)."
//
//  Strategy:
//    - Two independent source Properties `a`, `b`.
//    - A `selector` Property<bool> chooses which one the Computed
//      reads.
//    - Random walk: flip selector / write a / write b, each step.
//    - Invariant after every step: writing the UNREAD source MUST NOT
//      change the Computed's recompute counter.
// ============================================================================

#include <doctest/doctest.h>

#include "aria/computed.hpp"
#include "aria/property.hpp"
#include "fuzz_support.hpp"

using namespace aria;

TEST_CASE("L-17 fuzz: dynamic dependency drops unread branch under random walks") {
    fuzz::Rng rng{fuzz::seed(0xC0DE'17'17)};

    Property<int>  a{0};
    Property<int>  b{0};
    Property<bool> use_a{true};

    Computed<int> c{[&] {
        return use_a.get() ? a.get() : b.get();
    }};
    int recompute_hits = 0;
    auto sub = c.on_changed([&](int) { ++recompute_hits; });

    int last_seen = c.get();   // prime
    (void)last_seen;

    for (std::size_t step = 0; step < fuzz::iters(); ++step) {
        const bool selector_was_a = use_a.peek();
        const int  before_a = a.peek();
        const int  before_b = b.peek();
        const int  before_c = c.get();
        const int  before_hits = recompute_hits;

        const std::uint32_t op = rng.u32(0, 2);
        switch (op) {
            case 0: {
                // Flip the selector. The Computed's dep set may
                // re-attach to the other source on next pull.
                use_a.set(!selector_was_a);
                break;
            }
            case 1: {
                // Write the source the selector currently reads.
                if (selector_was_a) a.set(before_a + 1);
                else                b.set(before_b + 1);
                break;
            }
            case 2: {
                // Write the source the selector currently DOES NOT read.
                // Per L-17 this MUST NOT cause the Computed to fire.
                if (selector_was_a) b.set(before_b + 1);
                else                a.set(before_a + 1);
                break;
            }
        }

        // Pull `c` to make any pending recompute happen now.
        const int after_c    = c.get();
        const int after_hits = recompute_hits;

        if (op == 2) {
            // Wrote the unread branch -> Computed must not fire.
            CHECK(after_hits == before_hits);
            CHECK(after_c    == before_c);
        }
    }
}
