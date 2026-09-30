#include <doctest/doctest.h>
#include "../src/sse_wait.hpp"

namespace {
struct TestOutbox {
    std::mutex mu;
    std::coroutine_handle<> waiter;
    bool ready = false;
    [[nodiscard]] bool ready_locked() const noexcept { return ready; }
};
}

TEST_CASE("SSE wait publishes its waiter only while the outbox is empty") {
    TestOutbox client;
    aria::adapters::http::detail::SseWait wait{&client};
    CHECK_FALSE(wait.await_ready());
    CHECK(wait.await_suspend(std::noop_coroutine()));
    CHECK(client.waiter == std::noop_coroutine());
}

TEST_CASE("SSE arrival between readiness check and suspension continues without holding the lock") {
    TestOutbox client;
    aria::adapters::http::detail::SseWait wait{&client};
    REQUIRE_FALSE(wait.await_ready());
    // Deterministically model push/close winning the race before parking.
    {
        std::scoped_lock lock(client.mu);
        client.ready = true;
    }
    CHECK_FALSE(wait.await_suspend(std::noop_coroutine()));
    CHECK_FALSE(client.waiter);
    // The resumed consumer must be able to drain the queue immediately.
    std::unique_lock lock(client.mu, std::try_to_lock);
    CHECK(lock.owns_lock());
}
