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
//  Why this file exists
//  --------------------
//  docs/reference/lifecycle.md L-36 has listed this fuzzer in its coverage
//  table for a long time, but the file did not exist — L-36 was the one
//  entry in that table with no implementation behind it, and `test_scope.cpp`
//  covers only sequential single-task scenarios (seven cases, zero
//  concurrent launch/cancel/join interleavings).
//
//  Honest scope note: the `await_suspend`-resumes-on-the-current-stack
//  defect that prompted writing this file is NOT detected by these cases,
//  and was verified not to be — the suite passes under ASan+UBSan against
//  the buggy version too. That defect is latent rather than active: nothing
//  touches the coroutine frame after the in-stack resume today (the
//  awaiter's `await_resume()` is empty), so the standard violation does not
//  currently manifest. It was fixed because it is a contract violation one
//  edit away from becoming real, not because a sanitizer caught it. What
//  these cases DO pin down is the accounting and wakeup half of L-36, which
//  previously had no coverage at all.
//
//  Strategy
//  --------
//  Three complementary shapes, all driven off `fuzz::iters()`:
//
//    1. Random launch counts drained through `cancel_and_join()`, checking
//       exact accounting every iteration.
//    2. `co_await scope.join()` from a driver coroutine, with the number of
//       in-flight tasks varied so the awaiter hits BOTH paths: the
//       already-drained fast path (`await_ready` true / `await_suspend`
//       declining to suspend) and the genuinely-parked path.
//    3. Scope destruction racing pending work, to prove the dtor's
//       cancel-and-drain leaves nothing behind.
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

}  // namespace

TEST_CASE("L-36 fuzz: launch/cancel_and_join accounting converges to zero") {
    fuzz::Rng rng{fuzz::seed(0x5C09ED701)};

    auto entries = std::make_shared<std::atomic<std::uint64_t>>(0);
    auto exits   = std::make_shared<std::atomic<std::uint64_t>>(0);

    for (std::size_t step = 0; step < fuzz::iters(); ++step) {
        const std::uint32_t n = rng.u32(0, 4);
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

    auto entries = std::make_shared<std::atomic<std::uint64_t>>(0);
    auto exits   = std::make_shared<std::atomic<std::uint64_t>>(0);

    // Cap: each iteration spins up a fresh executor + scope + driver
    // coroutine, which is heavier than the tight loop above.
    const std::size_t capped = std::min(fuzz::iters(), std::size_t{20'000});

    for (std::size_t step = 0; step < capped; ++step) {
        VirtualTimeExecutor vt;
        auto resumed = std::make_shared<std::atomic<bool>>(false);

        {
            CoroutineScope scope;

            // n == 0 exercises the already-drained path, where
            // `await_ready()` is true or `await_suspend` must decline to
            // suspend by returning false. That is precisely the path that
            // used to resume on the wrong stack.
            const std::uint32_t n = rng.u32(0, 3);
            for (std::uint32_t i = 0; i < n; ++i) {
                scope.launch([entries, exits](CancellationToken tok) {
                    return counted_body(entries, exits, std::move(tok));
                });
            }

            auto driver = rng.coin(0.5) ? join_driver(scope, resumed)
                : join_existing_driver(scope, resumed);
            driver.start();
            vt.run_until_idle();

            // Whether or not the bodies had finished, the joiner must have
            // been handed back control: either inline (already drained) or
            // by the last task out.
            CHECK(resumed->load(std::memory_order_acquire));
            CHECK(scope.inflight_count() == 0);
        }
    }

    CHECK(entries->load() == exits->load());
}

TEST_CASE("L-36 fuzz: scope destruction drains pending work") {
    fuzz::Rng rng{fuzz::seed(0x5C09ED7012)};

    auto entries = std::make_shared<std::atomic<std::uint64_t>>(0);
    auto exits   = std::make_shared<std::atomic<std::uint64_t>>(0);

    const std::size_t capped = std::min(fuzz::iters(), std::size_t{20'000});

    for (std::size_t step = 0; step < capped; ++step) {
        // No explicit join: the dtor must cancel and drain. Leaking here
        // would trip the scope's own leak diagnostic.
        CoroutineScope scope;
        const std::uint32_t n = rng.u32(0, 4);
        for (std::uint32_t i = 0; i < n; ++i) {
            scope.launch([entries, exits](CancellationToken tok) {
                return counted_body(entries, exits, std::move(tok));
            });
        }
        if (rng.coin(0.3)) scope.cancel();
        // dtor runs here.
    }

    CHECK(entries->load() == exits->load());
}
