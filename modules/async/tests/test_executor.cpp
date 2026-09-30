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
