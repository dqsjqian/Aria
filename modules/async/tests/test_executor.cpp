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
