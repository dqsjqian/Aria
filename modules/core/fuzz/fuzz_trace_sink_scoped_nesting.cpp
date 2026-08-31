// ============================================================================
//  fuzz_trace_sink_scoped_nesting.cpp  (D-23)
// ----------------------------------------------------------------------------
//  Invariant under stress:
//    "ScopedTraceSink installs on construction and RESTORES the
//     previous state on destruction (which may be no sink or an outer
//     scoped sink). This lets tests nest in parallel without bleeding
//     into one another."
//
//  Restated as something checkable: at any depth of nesting, exactly
//  the INNERMOST live sink receives an event, and unwinding must
//  re-expose each enclosing sink in strict reverse order — including
//  restoring "no sink at all" at depth zero.
//
//  Why fuzz: nesting bugs are depth- and shape-dependent. A save /
//  restore implemented as "clear on destruction" passes at depth 1 and
//  fails at depth 2; one that restores the wrong slot passes uniformly
//  ordered nesting and fails when a bare `install_trace_sink` is mixed
//  into the middle of a scoped stack. Both shapes are generated here.
//
//  Strategy:
//    - Build a random-depth stack of ScopedTraceSink, each tagged with
//      its depth. After each push, publish once and assert the event
//      landed on THAT depth and nowhere else.
//    - Randomly interleave a bare install_trace_sink() at some depth.
//      Per D-23 each scope restores what IT saved at construction, so
//      the bare sink is what every deeper scope saved — it stays
//      visible while those unwind, and disappears only when the scope
//      it was installed inside unwinds. The expected sink per level is
//      therefore computed, not assumed: mixing a bare install into a
//      scoped stack is precisely where "restore" is easy to get wrong.
//    - Unwind and assert, at every level, that the newly exposed sink
//      is the one that level saved, and that depth zero has no sink.
// ============================================================================

#include <doctest/doctest.h>

#include "aria/diagnostics.hpp"
#include "fuzz_support.hpp"

#include <cstddef>
#include <deque>
#include <memory>
#include <optional>

using namespace aria;

namespace {

/// Records which depth tag last received an event, and how many
/// deliveries each depth saw.
struct Observed {
    int         last_tag = -1;
    std::size_t deliveries = 0;

    void reset() {
        last_tag = -1;
        deliveries = 0;
    }
};

TraceSink tagging_sink(int tag, Observed& obs) {
    return [tag, &obs](const TraceEvent&) {
        obs.last_tag = tag;
        ++obs.deliveries;
    };
}

void publish_one() {
    publish_trace(TraceCategory::Command, trace::Command{"execute"});
}

}  // namespace

TEST_CASE("D-23 fuzz: nested ScopedTraceSink restores in strict reverse order") {
    fuzz::Rng rng{fuzz::seed(0xD23'5C0'9ED)};

    // Each iteration builds and unwinds a whole stack, so cap the
    // count: depth up to 16 means up to 16 heap-allocated sinks and
    // 32 publishes per step.
    const std::size_t steps = std::min(fuzz::iters(), std::size_t{20'000});

    Observed obs;

    for (std::size_t step = 0; step < steps; ++step) {
        REQUIRE_FALSE(has_trace_sink());   // clean slate at depth 0

        const int depth = static_cast<int>(rng.u32(2, 16));
        // Depth at which a bare install_trace_sink() is injected, or -1
        // for none. Restricted to depth-2 so there is always a level
        // ABOVE it to save the bare sink; injecting at the innermost
        // level is a degenerate case (nothing saves it, so
        // stack[bare_at]'s own destructor overwrites it immediately)
        // and would only test the plain d-1 path again.
        const int bare_at = rng.coin(0.35)
                                ? static_cast<int>(rng.u32(0, static_cast<std::uint32_t>(depth - 2)))
                                : -1;

        // `ScopedTraceSink` deletes both copy and move (its whole point
        // is that the slot is restored exactly once, from exactly one
        // owner), so a std::vector cannot hold it — vector requires
        // Cpp17MoveInsertable even when it never reallocates. A deque
        // constructs elements in place and never relocates them, and
        // `optional` gives us explicit reverse-order destruction rather
        // than whatever order the container itself would pick.
        std::deque<std::optional<ScopedTraceSink>> stack;

        for (int d = 0; d < depth; ++d) {
            stack.emplace_back(std::in_place, tagging_sink(d, obs));

            obs.reset();
            publish_one();
            // Exactly the innermost sink ran, exactly once.
            CHECK(obs.deliveries == 1);
            CHECK(obs.last_tag == d);

            if (d == bare_at) {
                // Bare install at this depth: it becomes the current
                // sink, but the ScopedTraceSink at this level saved
                // the PREVIOUS one, so unwinding discards this.
                install_trace_sink(tagging_sink(1000 + d, obs));
                obs.reset();
                publish_one();
                CHECK(obs.deliveries == 1);
                CHECK(obs.last_tag == 1000 + d);
            }
        }

        // Unwind innermost-first, asserting each restore.
        //
        // Expected sink after destroying stack[d]: whatever stack[d]
        // saved at ITS construction time, i.e. the sink that was
        // current one level out. That is normally tag d-1.
        //
        // The bare install at `bare_at` was current only between
        // stack[bare_at]'s construction and stack[bare_at+1]'s, so
        // exactly ONE scope saved it — stack[bare_at+1]. It therefore
        // reappears once, when that level unwinds, and nowhere else:
        //
        //   * destroying stack[bare_at + 1] -> bare sink (1000+bare_at)
        //   * every other level d           -> tag d-1 (none at d == 0)
        //
        // This was verified against the implementation rather than
        // reasoned about: an earlier version of this fuzzer assumed the
        // bare sink stayed visible for every level deeper than
        // `bare_at`, which happens to be true only when
        // `bare_at + 1 == depth - 1`. That is precisely the off-by-one
        // -level confusion a nesting fuzzer should be catching, so the
        // expectation is computed per level.
        for (int d = depth - 1; d >= 0; --d) {
            stack[static_cast<std::size_t>(d)].reset();   // ~ScopedTraceSink

            obs.reset();
            publish_one();

            const bool bare_exposed = (bare_at >= 0 && d == bare_at + 1);
            const int  expected_tag = bare_exposed ? 1000 + bare_at : d - 1;

            if (expected_tag < 0) {
                // Depth 0 restored means "no sink at all" — the state
                // that existed before the outermost scope.
                CHECK(obs.deliveries == 0);
                CHECK_FALSE(has_trace_sink());
            } else {
                CHECK(obs.deliveries == 1);
                CHECK(obs.last_tag == expected_tag);
                CHECK(has_trace_sink());
            }
        }

        CHECK_FALSE(has_trace_sink());
    }
}
