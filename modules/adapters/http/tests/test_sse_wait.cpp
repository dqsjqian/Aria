#include <doctest/doctest.h>
#include "../src/sse_wait.hpp"
#include <mira/core/task_scope.hpp>
#include <functional>
#include <thread>
#include <utility>

namespace {
struct InterceptedMutex {
    std::mutex inner;
    std::function<void()> after_unlock;
    void lock() { inner.lock(); }
    void unlock() {
        inner.unlock();
        if (auto hook = std::exchange(after_unlock, {})) hook();
    }
};

template<class Mutex = std::mutex>
struct TestOutbox {
    Mutex mu;
    std::stop_source wake_source{std::nostopstate};
    bool ready = false;
    [[nodiscard]] bool ready_locked() const noexcept { return ready; }

    void publish() {
        std::stop_source wake{std::nostopstate};
        {
            std::lock_guard lock(mu);
            ready = true;
            wake = wake_source;
        }
        wake.request_stop();
    }
};
}

TEST_CASE("SSE wait consumes a ready outbox without parking or retaining its lock") {
    auto created = Mira::EventLoop::create();
    REQUIRE(created.has_value());
    auto& loop = *created;
    TestOutbox<> client;
    client.publish();
    auto root = [&]() -> Mira::Task<void> {
        co_await aria::adapters::http::detail::wait_for_sse(&client, loop);
        CHECK(loop.outstanding() == 0);
        std::unique_lock lock(client.mu, std::try_to_lock);
        CHECK(lock.owns_lock());
    };
    CHECK(loop.run_until_complete(root()).has_value());
}

TEST_CASE("SSE publication between the outbox check and timer registration is not lost") {
    auto created = Mira::EventLoop::create();
    REQUIRE(created.has_value());
    auto& loop = *created;
    TestOutbox<InterceptedMutex> client;
    // Run a producer exactly after the wait releases its outbox lock, before
    // it can register the timer. publish() also proves the lock is released.
    client.mu.after_unlock = [&] { client.publish(); };
    auto root = [&]() -> Mira::Task<void> {
        co_await aria::adapters::http::detail::wait_for_sse(&client, loop);
        CHECK(client.ready);
        CHECK(loop.outstanding() == 0);
    };
    CHECK(loop.run_until_complete(root()).has_value());
}

TEST_CASE("SSE wait keeps shutdown outstanding after the heartbeat is cancelled") {
    auto created = Mira::EventLoop::create();
    REQUIRE(created.has_value());
    auto& loop = *created;
    TestOutbox<> client;
    std::stop_source server_stop;
    bool heartbeat_done = false;
    bool sse_done = false;
    auto heartbeat = [&]() -> Mira::Task<void> {
        auto result = co_await loop.sleep_until(Mira::EventLoop::Clock::time_point::max(),
                                               {.stop = server_stop.get_token()});
        CHECK_FALSE(result.has_value());
        heartbeat_done = true;
    };
    auto sse = [&]() -> Mira::Task<void> {
        co_await aria::adapters::http::detail::wait_for_sse(&client, loop);
        sse_done = true;
    };
    auto root = [&]() -> Mira::Task<void> {
        Mira::TaskScope scope;
        scope.spawn(heartbeat());
        scope.spawn(sse());
        CHECK(loop.outstanding() == 2);
        server_stop.request_stop();
        while (!heartbeat_done) co_await loop.yield();
        CHECK_FALSE(sse_done);
        // No heartbeat or socket I/O remains. A raw coroutine waiter would be
        // invisible here and let run_until_complete report an abandoned task.
        CHECK(loop.outstanding() == 1);
        std::thread producer([&] { client.publish(); });
        producer.join();
        co_await scope.join();
        CHECK(sse_done);
        CHECK(loop.outstanding() == 0);
    };
    CHECK(loop.run_until_complete(root()).has_value());
}
