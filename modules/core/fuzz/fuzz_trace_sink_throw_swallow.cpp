// ============================================================================
//  fuzz_trace_sink_throw_swallow.cpp  (D-22 / AD1)
// ----------------------------------------------------------------------------
//  Invariant under stress:
//    "publish_trace / publish_trace_unchecked wrap the call in
//     try { sink(ev); } catch (...) { swallow }. Sink throws are
//     swallowed; the business path continues unaffected."
//
//  Why fuzz rather than a single test case: the swallow must hold for
//  EVERY publish overload, for every exception type (including ones
//  that do not derive from std::exception), and it must not leak the
//  installed sink or corrupt the global slot. A one-shot test proves
//  the happy case; only repetition catches "the second throw after a
//  swap leaves the sink slot in a bad state".
//
//  Strategy:
//    - The sink throws on a random subset of invocations, cycling
//      through several exception types.
//    - The publisher records a monotonically increasing counter
//      immediately AFTER each publish call. If an exception escaped,
//      the counter would stop advancing (the loop would unwind), so
//      the final count is the assertion.
//    - Interleaved install/clear/scoped-restore keeps the slot moving
//      so a throw cannot be observed only in a steady state.
//    - `has_trace_sink()` must remain truthful after a throw: an
//      exception must not clear the slot as a side effect.
// ============================================================================

#include <doctest/doctest.h>

#include "aria/diagnostics.hpp"
#include "fuzz_support.hpp"

#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string>

using namespace aria;

namespace {

/// Deliberately NOT derived from std::exception — `catch (...)` is what
/// the contract promises, so a non-standard payload must be swallowed
/// just as reliably as a std::runtime_error.
struct AlienThrow {
    int code = 0;
};

}  // namespace

TEST_CASE("D-22 fuzz: sink throws never reach the business path") {
    fuzz::Rng rng{fuzz::seed(0xD22'5177'0BAD)};

    const std::size_t steps = fuzz::iters();

    std::size_t invocations = 0;
    std::size_t throws_raised = 0;
    // Advanced after every publish returns. Any escaped exception
    // stops this from reaching `steps`.
    std::size_t survived = 0;

    auto throwing_sink = [&](const TraceEvent&) {
        ++invocations;
        // Throw on roughly half the calls, cycling the payload type so
        // the catch-all is exercised beyond std::exception.
        const auto pick = invocations % 5;
        if (pick == 0) {
            ++throws_raised;
            throw std::runtime_error("sink-boom");
        }
        if (pick == 1) {
            ++throws_raised;
            throw AlienThrow{static_cast<int>(invocations)};
        }
        if (pick == 2) {
            ++throws_raised;
            throw std::string("string-throw");
        }
        // pick 3 / 4: return normally.
    };

    install_trace_sink(throwing_sink);

    for (std::size_t step = 0; step < steps; ++step) {
        // Rotate across the publish surface: each overload has its own
        // try/catch, so each needs covering.
        switch (rng.u32(0, 3)) {
        case 0:
            publish_trace(TraceCategory::Command, trace::Command{"execute"});
            break;
        case 1:
            publish_trace_unchecked(TraceCategory::Command,
                                    trace::Command{"execute"});
            break;
        case 2: {
            TraceEvent ev{TraceCategory::List,
                          TracePayload{trace::List{"Insert", 0, 0, 1}},
                          std::chrono::steady_clock::now(),
                          std::nullopt};
            publish_trace(ev);
            break;
        }
        default: {
            TraceEvent ev{TraceCategory::List,
                          TracePayload{trace::List{"Remove", 0, 0, 0}},
                          std::chrono::steady_clock::now(),
                          std::nullopt};
            publish_trace_unchecked(ev);
            break;
        }
        }
        ++survived;

        // A throw must not have cleared the slot as a side effect: the
        // sink is still installed and still reachable.
        if (!has_trace_sink()) {
            // Fail loudly rather than silently reinstalling — a
            // vanished sink is the bug, not something to paper over.
            FAIL("sink slot was cleared by a throwing sink (D-22)");
        }

        // Occasionally churn the slot so the throw path is not only
        // exercised in a steady state.
        if (rng.coin(0.01)) {
            ScopedTraceSink nested{throwing_sink};
            publish_trace(TraceCategory::Command, trace::Command{"execute"});
            ++survived;
            // nested restores the outer throwing sink on scope exit.
        }
    }

    clear_trace_sink();

    // The load-bearing assertion: every publish returned normally.
    CHECK(survived >= steps);
    // Sanity — the sink really did run and really did throw, otherwise
    // the test passes vacuously.
    CHECK(invocations > 0);
    CHECK(throws_raised > 0);
    CHECK_FALSE(has_trace_sink());
}
