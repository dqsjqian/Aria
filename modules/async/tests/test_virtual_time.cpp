#include <doctest/doctest.h>

#include "aria/async/virtual_time_executor.hpp"
#include "aria/async/retry.hpp"
#include "aria/async/task.hpp"

#include <stdexcept>
#include <vector>

using namespace aria::async;
using namespace std::chrono_literals;

// ── Free-function coroutine bodies ─────────────────────────────────────────
// We deliberately avoid capturing lambdas as coroutine bodies — when a
// lambda is invoked and immediately returns a coroutine, the closure object
// dies at end of expression, but the coroutine frame stores REFERENCES to
// the captures, not copies of the closure. Using free functions with
// explicit reference parameters keeps everything alive correctly.
// (See: cpp20-coroutine-pitfalls skill, pitfall #7.)

namespace {

Task<void> coro_schedule_then_set(VirtualTimeExecutor& vt,
                                  std::chrono::milliseconds delay,
                                  bool& flag) {
    co_await schedule_after(vt, delay);
    flag = true;
}

Task<int> coro_attempt(int& calls, int succeed_at) {
    ++calls;
    if (calls < succeed_at) throw std::runtime_error("transient");
    co_return 7;
}

Task<int> coro_always_throws(int& calls) {
    ++calls;
    throw std::runtime_error("nope");
    co_return 0;
}

Task<void> coro_run_retry_giving_up(int& calls, bool& got_error) {
    try {
        co_await retry(2, [&calls]() { return coro_always_throws(calls); });
    } catch (const std::runtime_error&) {
        got_error = true;
    }
}

Task<void> coro_run_retry_with_backoff(VirtualTimeExecutor& vt,
                                       int& calls,
                                       bool& ok) {
    auto v = co_await retry_with_backoff(
        4, 100ms, vt,
        [&calls]() { return coro_attempt(calls, 3); });
    CHECK(v == 7);
    ok = true;
}

}  // namespace

TEST_CASE("VirtualTimeExecutor: instant fire on advance") {
    VirtualTimeExecutor vt;
    bool fired = false;
    vt.post_after(500ms, [&] { fired = true; });

    vt.advance_by(499ms);
    CHECK_FALSE(fired);

    vt.advance_by(1ms);
    CHECK(fired);
}

TEST_CASE("VirtualTimeExecutor: deadline ordering preserved") {
    VirtualTimeExecutor vt;
    std::vector<int> order;
    vt.post_after(300ms, [&] { order.push_back(3); });
    vt.post_after(100ms, [&] { order.push_back(1); });
    vt.post_after(200ms, [&] { order.push_back(2); });

    CHECK(vt.advance_by(500ms) == 3);
    CHECK(order == std::vector{1, 2, 3});
}

TEST_CASE("VirtualTimeExecutor: tasks scheduling other tasks") {
    VirtualTimeExecutor vt;
    int hit = 0;
    vt.post_after(100ms, [&] {
        ++hit;
        vt.post_after(100ms, [&] { ++hit; });
    });
    vt.advance_by(100ms);
    CHECK(hit == 1);
    vt.advance_by(100ms);
    CHECK(hit == 2);
}

TEST_CASE("schedule_after with VirtualTimeExecutor (coroutine)") {
    VirtualTimeExecutor vt;
    bool done = false;

    auto t = coro_schedule_then_set(vt, 250ms, done);
    t.start();   // begin executing; first co_await suspends into vt's queue

    vt.advance_by(249ms);
    CHECK_FALSE(done);
    vt.advance_by(1ms);
    CHECK(done);
}

TEST_CASE("retry: succeeds on first attempt") {
    int calls = 0;
    auto t = retry(3, [&calls]() { return coro_attempt(calls, 1); });
    t.start();
    CHECK(calls == 1);
}

TEST_CASE("retry: succeeds on third attempt") {
    int calls = 0;
    auto t = retry(5, [&calls]() { return coro_attempt(calls, 3); });
    t.start();
    CHECK(calls == 3);
}

TEST_CASE("retry: gives up and rethrows") {
    int calls = 0;
    bool got = false;
    auto t = coro_run_retry_giving_up(calls, got);
    t.start();
    CHECK(calls == 2);
    CHECK(got);
}

TEST_CASE("retry_with_backoff: virtual time covers exponential delays") {
    VirtualTimeExecutor vt;
    int calls = 0;
    bool ok = false;

    auto t = coro_run_retry_with_backoff(vt, calls, ok);
    t.start();

    // Backoffs: 100ms, then 200ms — succeeds on 3rd attempt.
    vt.advance_by(100ms);
    vt.advance_by(200ms);
    CHECK(calls == 3);
    CHECK(ok);
}
