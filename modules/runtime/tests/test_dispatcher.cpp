#include <doctest/doctest.h>

#include "aria/runtime/dispatcher.hpp"
#include <atomic>
#include <chrono>
#include <thread>

using namespace aria::runtime;

TEST_CASE("SimpleDispatcher: post + pump runs callable") {
    SimpleDispatcher d;
    int n = 0;
    d.post([&]() { n = 42; });
    auto processed = d.pump();
    CHECK(processed == 1);
    CHECK(n == 42);
}

TEST_CASE("SimpleDispatcher: multiple posts run in order") {
    SimpleDispatcher d;
    std::vector<int> seq;
    d.post([&]() { seq.push_back(1); });
    d.post([&]() { seq.push_back(2); });
    d.post([&]() { seq.push_back(3); });
    d.pump();
    CHECK(seq == std::vector<int>{1, 2, 3});
}

TEST_CASE("SimpleDispatcher: post_delayed waits for time") {
    SimpleDispatcher d;
    std::atomic<bool> fired{false};
    d.post_delayed(std::chrono::milliseconds{50}, [&]() { fired = true; });
    d.pump();
    CHECK_FALSE(fired);  // not yet
    std::this_thread::sleep_for(std::chrono::milliseconds{80});
    d.pump();
    CHECK(fired);
}

TEST_CASE("SimpleDispatcher: cross-thread post is safe") {
    SimpleDispatcher d;
    std::atomic<int> n{0};
    std::thread producer([&]() {
        for (int i = 0; i < 100; ++i) d.post([&]() { n.fetch_add(1); });
    });
    producer.join();
    d.pump(std::chrono::milliseconds{200});
    CHECK(n.load() == 100);
}

TEST_CASE("main_dispatcher() returns a default if none installed") {
    auto& d = main_dispatcher();
    int n = 0;
    d.post([&]() { n = 1; });
    // We can't necessarily pump it (we don't know the type) — just verify post does not throw
    CHECK(n == 0);  // not yet pumped
}

// ── B1 regression: SimpleDispatcher::run_one used to drop tasks on race ──
//
// Before the fix, this sequence would deadlock the test:
//   1. Owner thread enters run_one(), queue is empty, delayed has one
//      far-future entry → wait_until(soonest_delayed).
//   2. Producer thread calls post(taskA) — cv.notify_one wakes us.
//   3. Buggy code only re-checked `delayed`, found the deadline was
//      still in the future, returned WITHOUT consuming `taskA`. The
//      caller was now blocked on a condition that never fires.
// The fixed implementation re-checks `queue` first and re-loops until
// either path actually has work for us.
TEST_CASE("SimpleDispatcher::run_one: post races a delayed wait without losing the task") {
    SimpleDispatcher d;
    // Schedule a far-future delayed task so run_one parks on its
    // wait_until. The deadline is well past the test's timeout — we
    // never expect it to fire on its own.
    d.post_delayed(std::chrono::seconds{60}, []{ /* never */ });

    std::atomic<bool> ran{false};
    std::thread runner([&]{
        d.run_one();   // would hang forever before the fix
        ran = true;
    });

    // Give the runner thread a moment to park inside cv.wait_until on
    // the far-future deadline.
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    // Now post an immediate task. The fixed run_one should observe it,
    // pop it from `queue`, run it, and exit cleanly.
    std::atomic<bool> fired{false};
    d.post([&]{ fired = true; });

    // Bounded wait — without the fix, runner stays stuck and `ran`
    // never flips. With the fix, runner returns within a few ms.
    auto start = std::chrono::steady_clock::now();
    while (!ran.load() &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds{2}) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    CHECK(ran.load());
    CHECK(fired.load());
    if (runner.joinable()) runner.join();
}
