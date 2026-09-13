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

TEST_CASE("main_dispatcher() returns an owning snapshot") {
    set_main_dispatcher(nullptr);
    auto snapshot = main_dispatcher();
    REQUIRE(snapshot);
    auto* simple = dynamic_cast<SimpleDispatcher*>(snapshot.get());
    REQUIRE(simple);
    int n = 0;
    snapshot->post([&] { n = 1; });
    set_main_dispatcher(std::make_shared<SimpleDispatcher>());
    CHECK(main_dispatcher() != snapshot);
    CHECK(simple->pump() == 1);
    CHECK(n == 1);
    set_main_dispatcher(nullptr);
}

TEST_CASE("main_dispatcher replacement releases old captures outside its mutex") {
    bool released = false;
    auto old = std::make_shared<SimpleDispatcher>();
    auto token = std::shared_ptr<int>(new int, [&](int* value) {
        delete value;
        CHECK(main_dispatcher());
        released = true;
    });
    old->post([token = std::move(token)] {});
    set_main_dispatcher(std::move(old));
    set_main_dispatcher(std::make_shared<SimpleDispatcher>());
    CHECK(released);
    set_main_dispatcher(nullptr);
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

    std::atomic<bool> fired{false};
    std::thread producer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        // Grow the delayed heap while the owner waits on its old top.
        for (int i = 0; i < 64; ++i)
            d.post_delayed(std::chrono::seconds{60}, [] {});
        d.post([&] { fired = true; });
    });
    d.run_one();
    producer.join();
    CHECK(fired.load());
}

TEST_CASE("SimpleDispatcher: callback can destroy its dispatcher during pump") {
    auto dispatcher = std::make_unique<SimpleDispatcher>();
    bool second = false;
    dispatcher->post([&] { dispatcher.reset(); });
    dispatcher->post([&] { second = true; });
    CHECK(dispatcher->pump() == 1);
    CHECK_FALSE(dispatcher);
    CHECK_FALSE(second);
}

TEST_CASE("SimpleDispatcher: discarded captures may post during teardown") {
    auto dispatcher = std::make_unique<SimpleDispatcher>();
    auto* raw = dispatcher.get();
    bool released = false;
    auto capture = std::shared_ptr<int>(new int, [&](int* value) {
        delete value;
        released = true;
        raw->post([] {});
    });
    dispatcher->post([capture = std::move(capture)] {});
    dispatcher.reset();
    CHECK(released);
}

TEST_CASE("SimpleDispatcher: extreme delay saturates without firing") {
    SimpleDispatcher dispatcher;
    bool fired = false;
    dispatcher.post_delayed(std::chrono::milliseconds::max(), [&] { fired = true; });
    CHECK(dispatcher.pump(std::chrono::milliseconds::max()) == 0);
    CHECK_FALSE(fired);
}

TEST_CASE("SimpleDispatcher: pump and run_one enforce the owner thread") {
    SimpleDispatcher dispatcher;
    dispatcher.post([] {});
    std::thread worker([&] {
        CHECK_THROWS_AS(dispatcher.pump(), std::logic_error);
        CHECK_THROWS_AS(dispatcher.run_one(), std::logic_error);
    });
    worker.join();
    CHECK(dispatcher.pump() == 1);
}
