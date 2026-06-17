// ============================================================================
//  fuzz_when_any_race.cpp  (race-aware awaiter coverage)
// ----------------------------------------------------------------------------
//  Invariants under stress:
//
//    A. `when_any(...)` chooses exactly ONE winner. The winner's
//       index lies in [0, N) and `Result.error == nullptr` iff the
//       winner completed normally. Repeated runs MUST NOT crash, hang
//       or leak coroutines under random task durations.
//
//    B. `with_timeout(timer, dur, factory, OnTimeout::Fail)` resolves
//       to either:
//         (i)  the inner factory's value -- if it finished before the
//              timer fired -- in which case the timer must NOT
//              "win",
//         (ii) a thrown TimeoutError -- if the timer fired first --
//              in which case the inner work has been told to cancel.
//       In either branch the operation MUST complete (no hang) and
//       MUST NOT crash.
//
//  These two faces share the same race-slot machinery in
//  `detail/race_slot.hpp`; this fuzzer hammers it with random delays
//  to expose any inner-done vs winner-fired race condition.
// ============================================================================

#include <doctest/doctest.h>

#include "aria/async/cancellation.hpp"
#include "aria/async/executor.hpp"
#include "aria/async/task.hpp"
#include "aria/async/timeout.hpp"
#include "aria/async/virtual_time_executor.hpp"
#include "aria/async/when_all.hpp"
#include "fuzz_support.hpp"

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <vector>

using namespace aria;
using namespace aria::async;
using namespace std::chrono_literals;

namespace {

// Coroutine that waits `delay_ms` virtual milliseconds before
// returning a tagged value. Used by both fuzzers below.
Task<int> delayed_value(VirtualTimeExecutor& vt,
                        VirtualTimeExecutor::duration delay,
                        int tag) {
    co_await schedule_after(vt, delay);
    co_return tag;
}

// Drive a `when_any` of N delayed values; assert the resolved
// winner's tag matches the SLOT it came from (we encode the slot
// index into the tag so a winner index lookup catches any cross-talk).
Task<void> drive_when_any(VirtualTimeExecutor& vt,
                          std::vector<VirtualTimeExecutor::duration> delays,
                          std::size_t* out_winner_index,
                          int* out_winner_tag) {
    std::vector<Task<int>> ts;
    ts.reserve(delays.size());
    for (std::size_t i = 0; i < delays.size(); ++i) {
        ts.push_back(delayed_value(vt, delays[i], static_cast<int>(100 + i)));
    }
    auto r = co_await when_any(std::move(ts));
    *out_winner_index = r.index;
    if (r.value.has_value()) {
        *out_winner_tag = *r.value;
    } else {
        *out_winner_tag = -1;
    }
    co_return;
}

// Drive a `with_timeout(..., OnTimeout::Fail)`: we give the inner
// work `inner_ms` of work to do and the timeout `deadline_ms`.
Task<void> drive_with_timeout_fail(VirtualTimeExecutor& vt,
                                   VirtualTimeExecutor::duration inner_delay,
                                   std::chrono::milliseconds deadline,
                                   bool* out_completed_value,
                                   bool* out_threw_timeout) {
    *out_completed_value = false;
    *out_threw_timeout   = false;
    try {
        int v = co_await with_timeout(
            vt, deadline,
            [&vt, inner_delay] { return delayed_value(vt, inner_delay, 7); },
            OnTimeout::Fail);
        if (v == 7) *out_completed_value = true;
    } catch (const TimeoutError&) {
        *out_threw_timeout = true;
    }
    co_return;
}

}  // namespace

// ----------------------------------------------------------------------------
//  Invariant A: when_any picks exactly one winner; tag matches slot
// ----------------------------------------------------------------------------
TEST_CASE("when_any: exactly one winner across random delay vectors") {
    fuzz::Rng rng{fuzz::seed(0x9'A'1'F'A'17)};

    // Each iteration races N (2..6) coroutines with random virtual
    // delays in [1..50] ms. After advancing virtual time past the
    // longest delay, exactly one winner MUST have been chosen and
    // its tag MUST match `100 + winner_index`.
    const std::size_t iters_capped = std::min(fuzz::iters(), std::size_t{20'000});
    for (std::size_t step = 0; step < iters_capped; ++step) {
        VirtualTimeExecutor vt;
        const std::uint32_t n = rng.u32(2, 6);
        std::vector<VirtualTimeExecutor::duration> delays;
        delays.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i) {
            delays.push_back(std::chrono::milliseconds{rng.u32(1, 50)});
        }

        std::size_t winner_index = std::size_t(-1);
        int         winner_tag   = -999;
        drive_when_any(vt, delays, &winner_index, &winner_tag).start_detached();

        // Advance enough to drain the slowest task.
        vt.advance_to(std::chrono::milliseconds{60});

        REQUIRE(winner_index < n);
        REQUIRE(winner_tag == static_cast<int>(100 + winner_index));
    }
}

// ----------------------------------------------------------------------------
//  Invariant B: with_timeout(..., OnTimeout::Fail) -- inner-done vs
//  timer-fire race resolves cleanly to exactly one branch
// ----------------------------------------------------------------------------
TEST_CASE("with_timeout(Fail): inner-done vs timer-fire race resolves cleanly") {
    fuzz::Rng rng{fuzz::seed(0xB1'7'F'A'117)};

    // Random pairs of (inner_delay, deadline). Cases:
    //   inner < deadline -> value branch must fire, timer must lose
    //   inner > deadline -> timeout branch must fire
    //   inner = deadline -> EITHER branch is acceptable (the race is
    //                        the whole point), but BOTH MUST NOT
    //                        fire and the operation MUST complete.
    const std::size_t iters_capped = std::min(fuzz::iters(), std::size_t{20'000});
    for (std::size_t step = 0; step < iters_capped; ++step) {
        VirtualTimeExecutor vt;
        const auto inner_ms    = std::chrono::milliseconds{rng.u32(1, 30)};
        const auto deadline_ms = std::chrono::milliseconds{rng.u32(1, 30)};

        bool got_value   = false;
        bool got_timeout = false;
        drive_with_timeout_fail(vt, inner_ms, deadline_ms,
                                &got_value, &got_timeout)
            .start_detached();

        // Drain virtual time past both points.
        vt.advance_to(std::chrono::milliseconds{60});

        // Exactly one branch fires. Logical contract: never both,
        // never neither.
        REQUIRE(got_value != got_timeout);

        // Strict inequality cases pin down which branch:
        if (inner_ms < deadline_ms) {
            REQUIRE(got_value);
        } else if (inner_ms > deadline_ms) {
            REQUIRE(got_timeout);
        }
        // inner_ms == deadline_ms : either is fine; the
        // never-both / never-neither check above suffices.
    }
}
