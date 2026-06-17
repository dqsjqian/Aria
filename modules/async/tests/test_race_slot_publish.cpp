// test_race_slot_publish.cpp — stress-test RaceSlot's two-phase
// publication protocol against the await_ready fast path.
//
// History:
//   The original single-atomic protocol stored the winner code BEFORE
//   `result` was written. A parent observing `winner != 0` via
//   `await_ready()` (which does NOT take `mu_`) could rush into
//   `await_resume()` and read an unpublished `result` — UB.
//
// What this test exercises:
//   * One "driver" thread loops: try_claim → write result → publish
//     → notify_winner_resume.
//   * One "parent" thread loops: poll await_ready; once it sees the
//     slot resolved, call await_resume and assert the value/index
//     match what the driver wrote.
//   * Iterate N times to maximise the probability of hitting the
//     try_claim/publish window if it were observable.
//
// Pass criterion (two-phase protocol):
//   await_ready returning true ⇒ result is fully written.
//   parent never observes a half-published slot.
//
// Counter-example (single-atomic protocol — what we used to have):
//   variant.index() would occasionally be 0 (monostate) inside
//   await_resume, triggering bad_variant_access.
//
// Note: doctest's CHECK macros are not always thread-safe under
// contention; we accumulate counters in atomics and assert once at
// the end of each round.

#include <doctest/doctest.h>

#include "aria/async/detail/race_slot.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <variant>

using aria::async::detail::RaceSlot;
using aria::async::detail::RaceSlotAwaiter;

namespace {

// Mimic exactly what when_any's driver does: claim → write index/result
// → publish → resume. We don't actually resume any coroutine here; we
// just want to observe the state transitions from a parallel "parent".
template<typename T>
void driver_publish(RaceSlot<T>& slot, std::size_t idx, T value) {
    if (slot.try_claim(/*winner=*/1)) {
        slot.winner_index = idx;
        slot.result.template emplace<1>(std::move(value));
        slot.publish(/*winner=*/1);
        // notify_winner_resume() requires a coroutine; we skip it
        // here because the parent in this test polls await_ready
        // directly instead of suspending.
    }
}

}  // namespace

TEST_CASE("RaceSlot publishes result atomically vs await_ready (int)") {
    constexpr int kIterations = 50000;

    std::atomic<int> bad_publish_count{0};
    std::atomic<int> good_publish_count{0};

    for (int it = 0; it < kIterations; ++it) {
        auto slot = std::make_shared<RaceSlot<int>>();

        const int  expected_value = it + 1;
        const auto expected_index = static_cast<std::size_t>(it);

        // Driver thread: write the slot.
        std::thread driver([&] {
            // Tiny artificial fence so the parent has a real chance
            // of catching the in-flight state. Without this the OS
            // tends to schedule the two threads sequentially.
            std::this_thread::yield();
            driver_publish<int>(*slot, expected_index, expected_value);
        });

        // Parent thread: spin on await_ready; once it sees the slot
        // ready, decode via await_resume and verify integrity.
        std::thread parent([&] {
            RaceSlotAwaiter<int> awaiter{slot};
            for (;;) {
                if (awaiter.await_ready()) {
                    // If two-phase publish is correct, the variant
                    // MUST be on index 1 (value) here. If the protocol
                    // were broken (single-atomic), we would sometimes
                    // see index 0 (monostate) and bad_variant_access
                    // would throw on get<1>.
                    if (slot->result.index() != 1) {
                        bad_publish_count.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }
                    int v = awaiter.await_resume();
                    if (v != expected_value ||
                        slot->winner_index != expected_index) {
                        bad_publish_count.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        good_publish_count.fetch_add(1, std::memory_order_relaxed);
                    }
                    return;
                }
                // No yield/sleep here on purpose — we want a tight
                // spin so the await_ready snapshot lands close to
                // the driver's publish.
            }
        });

        driver.join();
        parent.join();
    }

    CHECK(bad_publish_count.load() == 0);
    CHECK(good_publish_count.load() == kIterations);
}

TEST_CASE("RaceSlot publishes winner_index atomically vs await_ready (string)") {
    // Same shape, but with a non-trivial value type to exercise the
    // case where `result.emplace<1>` is not a single-instruction store.
    constexpr int kIterations = 20000;

    std::atomic<int> bad_publish_count{0};
    std::atomic<int> good_publish_count{0};

    for (int it = 0; it < kIterations; ++it) {
        auto slot = std::make_shared<RaceSlot<std::string>>();

        const std::string expected_value = "round-" + std::to_string(it);
        const auto        expected_index = static_cast<std::size_t>(it);

        std::thread driver([&] {
            std::this_thread::yield();
            driver_publish<std::string>(*slot, expected_index, expected_value);
        });

        std::thread parent([&] {
            RaceSlotAwaiter<std::string> awaiter{slot};
            for (;;) {
                if (awaiter.await_ready()) {
                    if (slot->result.index() != 1) {
                        bad_publish_count.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }
                    std::string v = awaiter.await_resume();
                    if (v != expected_value ||
                        slot->winner_index != expected_index) {
                        bad_publish_count.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        good_publish_count.fetch_add(1, std::memory_order_relaxed);
                    }
                    return;
                }
            }
        });

        driver.join();
        parent.join();
    }

    CHECK(bad_publish_count.load() == 0);
    CHECK(good_publish_count.load() == kIterations);
}

TEST_CASE("RaceSlot late-claim losers silently drop") {
    // Smoke test for the loser-disengagement contract.
    auto slot = std::make_shared<RaceSlot<int>>();

    // Winner claims first.
    REQUIRE(slot->try_claim(/*code=*/1));
    slot->winner_index = 7;
    slot->result.emplace<1>(42);
    slot->publish(/*code=*/1);

    // Two losers attempt to claim afterwards.
    CHECK_FALSE(slot->try_claim(/*code=*/2));
    CHECK_FALSE(slot->try_claim(/*code=*/99));

    // Winner's view survives.
    CHECK(slot->result.index() == 1);
    CHECK(std::get<1>(slot->result) == 42);
    CHECK(slot->winner_index == 7);
    CHECK(slot->winner.load(std::memory_order_acquire) == 1);
}
