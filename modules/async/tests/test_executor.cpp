#include <doctest/doctest.h>

#include "aria/async/executor.hpp"
#include <atomic>
#include <chrono>
#include <thread>

using namespace aria::async;

TEST_CASE("InlineExecutor: runs synchronously") {
    InlineExecutor exec;
    int n = 0;
    exec.post([&]() { n = 42; });
    CHECK(n == 42);
}

TEST_CASE("ThreadPoolExecutor: runs tasks across threads") {
    ThreadPoolExecutor exec(4);
    CHECK(exec.worker_count() == 4);

    std::atomic<int> counter{0};
    for (int i = 0; i < 100; ++i) {
        exec.post([&]() { counter.fetch_add(1); });
    }

    // Wait until all 100 tasks have run
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (counter.load() < 100 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(counter.load() == 100);
}

namespace {
struct ExecutorClearCapture {
    MainThreadExecutor* executor;
    ~ExecutorClearCapture() { executor->post([] {}); }
};

struct ExecutorTeardownCapture {
    MainThreadExecutor* executor;
    std::shared_ptr<int> lifetime;
    ExecutorTeardownCapture(MainThreadExecutor* exec, std::shared_ptr<int> keep)
        : executor(exec), lifetime(std::move(keep)) {}
    ExecutorTeardownCapture(const ExecutorTeardownCapture&) = delete;
    ExecutorTeardownCapture& operator=(const ExecutorTeardownCapture&) = delete;
    ExecutorTeardownCapture(ExecutorTeardownCapture&&) = delete;
    ExecutorTeardownCapture& operator=(ExecutorTeardownCapture&&) = delete;
    ~ExecutorTeardownCapture() {
        try { executor->post([keep = lifetime] { (void)keep; }); }
        catch (...) { aria::report_callback_failure("test.executor.teardown", std::current_exception()); }
    }
};
}

TEST_CASE("MainThreadExecutor: clear releases reentrant captures outside its lock") {
    MainThreadExecutor executor;
    auto capture = std::make_shared<ExecutorClearCapture>(&executor);
    executor.post([capture] {});
    capture.reset();
    executor.clear();
    CHECK(executor.pending() == 1);
    CHECK(executor.drain() == 1);
}

// Doctest macro internals introduce the flagged branches/type traits.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("MainThreadExecutor: rejects all pumping and clearing from a non-owner") {
    MainThreadExecutor executor;
    executor.drain();
    int called = 0;
    executor.post([&] { ++called; });
    std::atomic<int> rejected{0};
    std::thread foreign([&] {
        auto check = [&](auto operation) {
            try { operation(); }
            catch (const std::logic_error&) { ++rejected; }
        };
        check([&] { executor.drain(); });
        check([&] { executor.run_one(); });
        check([&] { executor.pump_until([] { return true; }); });
        check([&] { executor.clear(); });
    });
    foreign.join();
    CHECK(rejected.load() == 4);
    CHECK(called == 0);
    CHECK(executor.pending() == 1);
    CHECK(executor.drain() == 1);
    CHECK(called == 1);
}

TEST_CASE("MainThreadExecutor: single callback drains its posted successors") {
    MainThreadExecutor executor;
    std::vector<int> order;
    executor.post([&] {
        order.push_back(1);
        executor.post([&] { order.push_back(2); });
    });
    CHECK(executor.drain() == 2);
    CHECK(order == std::vector<int>{1, 2});
    CHECK(executor.pending() == 0);
}

TEST_CASE("MainThreadExecutor: detached batch remains ahead of reentrant writes") {
    MainThreadExecutor executor;
    std::vector<int> order;
    executor.post([&] {
        order.push_back(1);
        executor.post([&] { order.push_back(3); });
        executor.clear();
        executor.post([&] { order.push_back(4); });
    });
    executor.post([&] { order.push_back(2); });
    CHECK(executor.drain() == 3);
    CHECK(order == std::vector<int>{1, 2, 4});
}

TEST_CASE("MainThreadExecutor: foreign producers wake both bound and unbound owners") {
    MainThreadExecutor executor;
    bool done = false;
    std::thread producer([&] { executor.post([&] { done = true; }); });
    CHECK(executor.pump_until([&] { return done; }));
    producer.join();
    done = false;
    std::thread second([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        executor.post([&] { done = true; });
    });
    CHECK(executor.pump_until([&] { return done; }));
    second.join();
}

TEST_CASE("MainThreadExecutor: single callback failure does not strand posted work") {
    MainThreadExecutor executor;
    int calls = 0;
    executor.post([&] {
        executor.post([&] { ++calls; });
        throw std::runtime_error("single dispatch failure");
    });
    CHECK(executor.drain() == 2);
    CHECK(calls == 1);
}

TEST_CASE("MainThreadExecutor: teardown safely rejects posts from capture destructors") {
    auto lifetime = std::make_shared<int>(0);
    std::weak_ptr<int> weak = lifetime;
    {
        MainThreadExecutor executor;
        auto capture = std::make_shared<ExecutorTeardownCapture>(
            &executor, std::move(lifetime));
        executor.post([keep = std::move(capture)] { (void)keep; });
        CHECK_FALSE(weak.expired());
    }
    CHECK(weak.expired());
}
