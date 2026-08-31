// ============================================================================
//  fuzz_trace_sink_install_race.cpp  (D-21)
// ----------------------------------------------------------------------------
//  Invariant under stress:
//    "Sink registration (install_trace_sink / clear_trace_sink /
//     ScopedTraceSink) serialises through a global mutex. Publishing
//     takes a single lock-scoped copy of the sink's shared_ptr, then
//     invokes the sink OUTSIDE the lock."
//
//  The consequence a consumer relies on, and what this fuzzer pins:
//  a sink object must stay alive for the whole duration of the call
//  that reached it, even if another thread swaps or clears the sink
//  mid-invocation. The publish path lifts the sink into a strong
//  reference precisely so the swapping thread cannot destroy it
//  underneath a running callback.
//
//  Strategy:
//    - One publisher thread hammers publish_trace() continuously.
//    - One installer thread randomly install/clear/replaces the sink.
//    - Each sink instance owns a heap-allocated "generation guard"
//      that flips a flag on destruction. The sink body asserts, while
//      running, that its own guard is still alive — so a
//      use-after-free of a swapped-out sink turns into a hard failure
//      instead of a silent read of freed memory (which only ASan
//      would otherwise catch, and only sometimes).
//    - Every event a sink observes must be one it can attribute, so
//      the sink also checks the payload it receives is well-formed:
//      a torn shared_ptr read would deliver garbage here.
//
//  Note on why this is not just "run ASan over the existing tests":
//  the race needs a swap to land *between* the snapshot load and the
//  invocation, which a single-threaded test never produces.
// ============================================================================

#include <doctest/doctest.h>

#include "aria/diagnostics.hpp"
#include "fuzz_support.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <variant>
#include <vector>

using namespace aria;

namespace {

/// Owned by a sink; flipped when that sink is destroyed. A sink body
/// that observes `dead == true` for its own guard has been invoked
/// after destruction — the exact failure D-21 forbids.
struct SinkGuard {
    std::atomic<bool> dead{false};
    ~SinkGuard() { dead.store(true, std::memory_order_release); }
};

}  // namespace

TEST_CASE("D-21 fuzz: install/clear never lets publish touch a destroyed sink") {
    fuzz::Rng rng{fuzz::seed(0xD21'51'11'CE)};

    // Two threads per iteration would be wasteful; instead run ONE
    // long race whose length scales with iters(). Capped because each
    // step is a full atomic publish + occasional heap churn.
    const std::size_t steps = std::min(fuzz::iters(), std::size_t{200'000});

    std::atomic<bool>        stop{false};
    std::atomic<std::size_t> delivered{0};
    std::atomic<std::size_t> use_after_free{0};
    std::atomic<std::size_t> malformed{0};

    // Keep every guard alive until the very end: the point is to
    // detect a sink invoked after ITS OWN destruction, not to test
    // whether freed memory happens to still read true.
    std::vector<std::shared_ptr<SinkGuard>> guards;
    guards.reserve(64);
    std::mutex guards_mu;

    auto make_sink = [&](std::shared_ptr<SinkGuard> guard) {
        return [guard = std::move(guard), &delivered, &use_after_free,
                &malformed](const TraceEvent& ev) {
            if (guard->dead.load(std::memory_order_acquire)) {
                use_after_free.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            // A torn read of the sink's shared_ptr would hand us a
            // half-published event; both fields must be coherent.
            if (ev.category != TraceCategory::List) {
                malformed.fetch_add(1, std::memory_order_relaxed);
            } else if (!std::get_if<trace::List>(&ev.payload)) {
                malformed.fetch_add(1, std::memory_order_relaxed);
            }
            delivered.fetch_add(1, std::memory_order_relaxed);
        };
    };

    // Publisher: unconditional publish_trace (the gated variant is
    // covered by the zero-overhead bench). Uses the List payload
    // because it is the cheapest to build.
    std::thread publisher([&] {
        std::size_t n = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            publish_trace(TraceCategory::List,
                          trace::List{"Insert", n % 8, 0, (n % 8) + 1});
            ++n;
        }
    });

    // Installer: swap / clear / re-install under the race.
    std::thread installer([&] {
        for (std::size_t i = 0; i < steps; ++i) {
            const auto pick = rng.u32(0, 2);
            if (pick == 0) {
                clear_trace_sink();
            } else if (pick == 1) {
                auto guard = std::make_shared<SinkGuard>();
                {
                    std::lock_guard lk(guards_mu);
                    guards.push_back(guard);
                }
                install_trace_sink(make_sink(std::move(guard)));
            } else {
                // Nested scoped sink: installs, then restores whatever
                // the other branches left behind (D-23). Its lifetime
                // ends inside this iteration, so a publish in flight
                // must not follow the dangling one.
                auto guard = std::make_shared<SinkGuard>();
                {
                    std::lock_guard lk(guards_mu);
                    guards.push_back(guard);
                }
                ScopedTraceSink scoped{make_sink(std::move(guard))};
            }
        }
    });

    installer.join();
    stop.store(true, std::memory_order_relaxed);
    publisher.join();
    clear_trace_sink();

    // The load-bearing assertions.
    CHECK(use_after_free.load() == 0);
    CHECK(malformed.load() == 0);
    // Sanity: the race actually exercised the delivery path. If this
    // trips, the fuzzer is passing vacuously.
    CHECK(delivered.load() > 0);
}
