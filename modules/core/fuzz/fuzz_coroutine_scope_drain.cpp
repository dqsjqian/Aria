// ============================================================================
//  fuzz_coroutine_scope_drain.cpp  (L-36)
// ----------------------------------------------------------------------------
//  Invariant under stress:
//    "CoroutineScope is a real structured-concurrency primitive. Under any
//     interleaving of launch / cancel / join:
//       * accounting is exact — every launch that increments `inflight`
//         has a matching decrement, and `inflight_count()` converges to 0
//         once the scope has drained;
//       * no callback is missed — a joiner that suspends is always
//         resumed;
//       * nothing is resumed twice, and no coroutine frame leaks."
//
//  Strategy
//  --------
//  Three complementary shapes, all driven off `fuzz::iters()`:
//
//    1. Preserve the synchronous launch/accounting baseline.
//    2. Park children on virtual deadlines, register two joiners before the
//       last child exits, and verify neither resumes before that last exit.
//    3. Destroy a scope whose children are waiting for cancellation, then
//       check every child observed cancellation and released its frame.
//
//  Everything runs on ManualExecutor-style deterministic pumping via
//  VirtualTimeExecutor, so a failure reproduces from the seed alone.
// ============================================================================

#include <doctest/doctest.h>

#include "aria/async/cancellation.hpp"
#include "aria/async/executor.hpp"
#include "aria/async/scope.hpp"
#include "aria/async/task.hpp"
#include "aria/async/virtual_time_executor.hpp"
#include "fuzz_support.hpp"

#include <atomic>
#include <cstdint>
#include <memory>

using namespace aria;
using namespace aria::async;

namespace {

// A body that records entry and exit through a side channel that outlives
// the scope. Free function (not a lambda) so the coroutine frame owns its
// parameters — a coroutine inside a lambda body reads captures through the
// lambda's `this`, which does not survive.
Task<void> counted_body(std::shared_ptr<std::atomic<std::uint64_t>> entries,
                        std::shared_ptr<std::atomic<std::uint64_t>> exits,
                        CancellationToken tok) {
    entries->fetch_add(1, std::memory_order_relaxed);
    // Probe the token the way cooperative work is supposed to: observing
    // cancellation is allowed, but the exit accounting must happen either
    // way, which is what the invariant below checks.
    if (!tok.is_cancelled()) {
        co_await std::suspend_never{};
    }
    exits->fetch_add(1, std::memory_order_relaxed);
}

// Driver that joins a scope from inside a coroutine, recording that it was
// actually resumed afterwards.
Task<void> join_driver(CoroutineScope& scope,
                       std::shared_ptr<std::atomic<bool>> resumed) {
    co_await scope.join();
    resumed->store(true, std::memory_order_release);
}

Task<void> join_existing_driver(CoroutineScope& scope,
                                std::shared_ptr<std::atomic<bool>> resumed) {
    co_await scope.join_existing();
    resumed->store(true, std::memory_order_release);
}

struct PendingCounts {
    std::size_t entries = 0;
    std::size_t exits = 0;
    std::size_t cancelled = 0;
};

Task<void> delayed_counted_body(VirtualTimeExecutor& timer,
                              std::chrono::milliseconds delay,
                              std::shared_ptr<PendingCounts> counts,
                              CancellationToken token) {
    ++counts->entries;
    co_await schedule_after(timer, delay);
    if (token.is_cancelled()) ++counts->cancelled;
    ++counts->exits;
}

Task<void> counted_until_cancelled(std::shared_ptr<PendingCounts> counts,
                                 CancellationToken token) {
    ++counts->entries;
    co_await token;
    if (token.is_cancelled()) ++counts->cancelled;
    ++counts->exits;
}

}  // namespace

TEST_CASE("L-36 fuzz: launch/cancel_and_join accounting converges to zero") {
    fuzz::Rng rng{fuzz::seed(0x5C09ED701)};

    auto entries = std::make_shared<std::atomic<std::uint64_t>>(0);
    auto exits   = std::make_shared<std::atomic<std::uint64_t>>(0);

    for (std::size_t step = 0; step < fuzz::iters(); ++step) {
        // Guarantee nonempty accounting even for ARIA_FUZZ_ITERS=1.
        const std::uint32_t n = step == 0 ? 1 : rng.u32(0, 4);
        {
            CoroutineScope scope;
            for (std::uint32_t i = 0; i < n; ++i) {
                scope.launch([entries, exits](CancellationToken tok) {
                    return counted_body(entries, exits, std::move(tok));
                });
            }
            // Sometimes cancel first, sometimes go straight to the join.
            if (rng.coin(0.5)) scope.cancel();

            const bool drained = scope.cancel_and_join();
            CHECK(drained);
            // Accounting must be exact the moment the join returns.
            CHECK(scope.inflight_count() == 0);
        }
    }

    // Every body that entered must have exited: no wrapper lost its
    // decrement, and no body was resumed twice.
    CHECK(entries->load() == exits->load());
    CHECK(entries->load() > 0);
}

TEST_CASE("L-36 fuzz: co_await join() resumes on both drained and parked paths") {
    fuzz::Rng rng{fuzz::seed(0x5C09EA11)};

    // Cap: each iteration spins up a fresh executor + scope + driver
    // coroutine, which is heavier than the tight loop above.
    const std::size_t capped = std::min(fuzz::iters(), std::size_t{20'000});

    for (std::size_t step = 0; step < capped; ++step) {
        VirtualTimeExecutor vt;
        auto counts = std::make_shared<PendingCounts>();
        {
            CoroutineScope scope;
            const std::uint32_t n = rng.u32(0, 3);
            for (std::uint32_t i = 0; i < n; ++i) {
                scope.launch([&vt, counts, i](CancellationToken tok) {
                    return delayed_counted_body(vt, std::chrono::milliseconds{i + 1},
                                                counts, std::move(tok));
                });
            }
            REQUIRE(counts->entries == n);
            REQUIRE(counts->exits == 0);
            REQUIRE(scope.inflight_count() == n);

            const bool cancel_on_join = rng.coin(0.5);
            auto resumed = std::make_shared<std::atomic<bool>>(false);
            auto second_resumed = std::make_shared<std::atomic<bool>>(false);
            auto driver = cancel_on_join ? join_driver(scope, resumed)
                                         : join_existing_driver(scope, resumed);
            auto second = join_existing_driver(scope, second_resumed);
            driver.start();
            second.start();
            CHECK(driver.done() == (n == 0));
            CHECK(second.done() == (n == 0));
            CHECK(resumed->load() == (n == 0));
            CHECK(second_resumed->load() == (n == 0));

            for (std::uint32_t finished = 1; finished <= n; ++finished) {
                vt.advance_by(std::chrono::milliseconds{1});
                CHECK(counts->exits == finished);
                CHECK(scope.inflight_count() == n - finished);
                CHECK(resumed->load() == (finished == n));
                CHECK(second_resumed->load() == (finished == n));
            }
            REQUIRE(driver.done());
            REQUIRE(second.done());
            driver.blocking_get();
            second.blocking_get();
            CHECK(scope.inflight_count() == 0);
            CHECK(counts->cancelled == (cancel_on_join ? n : 0));
            CHECK(vt.pending() == 0);
        }
        CHECK(counts.use_count() == 1);  // no child/owner frame retained its state
    }
}

TEST_CASE("L-36 fuzz: scope destruction drains pending work") {
    fuzz::Rng rng{fuzz::seed(0x5C09ED7012)};

    const std::size_t capped = std::min(fuzz::iters(), std::size_t{20'000});

    for (std::size_t step = 0; step < capped; ++step) {
        auto counts = std::make_shared<PendingCounts>();
        CancellationToken token;
        const std::uint32_t n = rng.u32(1, 4);
        {
            CoroutineScope scope;
            token = scope.token();
            for (std::uint32_t i = 0; i < n; ++i) {
                scope.launch([counts](CancellationToken tok) {
                    return counted_until_cancelled(counts, std::move(tok));
                });
            }
            REQUIRE(counts->entries == n);
            REQUIRE(counts->exits == 0);
            REQUIRE(scope.inflight_count() == n);
            CHECK_FALSE(token.is_cancelled());
            // No explicit cancel/join: every child is still parked at dtor.
        }
        CHECK(token.is_cancelled());
        CHECK(counts->exits == n);
        CHECK(counts->cancelled == n);
        CHECK(counts.use_count() == 1);
    }
}
