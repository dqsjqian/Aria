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

namespace {
struct VirtualClearCapture {
    VirtualTimeExecutor* executor;
    ~VirtualClearCapture() { executor->post([] {}); }
};
struct VirtualTeardownCapture {
    VirtualTimeExecutor* executor;
    std::shared_ptr<int> lifetime;
    VirtualTeardownCapture(VirtualTimeExecutor* exec, std::shared_ptr<int> keep)
        : executor(exec), lifetime(std::move(keep)) {}
    VirtualTeardownCapture(const VirtualTeardownCapture&) = delete;
    VirtualTeardownCapture& operator=(const VirtualTeardownCapture&) = delete;
    VirtualTeardownCapture(VirtualTeardownCapture&&) = delete;
    VirtualTeardownCapture& operator=(VirtualTeardownCapture&&) = delete;
    ~VirtualTeardownCapture() {
        try { executor->post([keep = lifetime] { (void)keep; }); }
        catch (...) { aria::report_callback_failure("test.virtual_time.teardown", std::current_exception()); }
    }
};
}

TEST_CASE("VirtualTimeExecutor: clear releases reentrant captures outside its lock") {
    VirtualTimeExecutor executor;
    auto capture = std::make_shared<VirtualClearCapture>(&executor);
    executor.post([capture] {});
    capture.reset();
    executor.clear();
    CHECK(executor.pending() == 1);
    CHECK(executor.run_until_idle() == 1);
}

TEST_CASE("VirtualTimeExecutor: teardown safely rejects posts from capture destructors") {
    auto lifetime = std::make_shared<int>(0);
    std::weak_ptr<int> weak = lifetime;
    {
        VirtualTimeExecutor executor;
        auto capture = std::make_shared<VirtualTeardownCapture>(
            &executor, std::move(lifetime));
        executor.post([keep = std::move(capture)] { (void)keep; });
        CHECK_FALSE(weak.expired());
    }
    CHECK(weak.expired());
}

// Doctest macro internals introduce the flagged branches/type traits.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("VirtualTimeExecutor: negative delays are immediate and time is monotonic") {
    VirtualTimeExecutor executor;
    executor.advance_by(10ms);
    auto observed = 0ms;
    executor.post_after(-20ms, [&] { observed = executor.now(); });
    CHECK(executor.advance_by(0ms) == 1);
    CHECK(observed == 10ms);
    CHECK(executor.now() == 10ms);
    // Doctest macro internals introduce the flagged branches/type traits.
    // NOLINTNEXTLINE(modernize-type-traits)
    CHECK_THROWS_AS(executor.advance_by(-1ms), std::invalid_argument);
    // Doctest macro internals introduce the flagged branches/type traits.
    // NOLINTNEXTLINE(modernize-type-traits)
    CHECK_THROWS_AS(executor.advance_to(9ms), std::invalid_argument);
    CHECK(executor.now() == 10ms);
}

// Doctest macro internals introduce the flagged branches/type traits.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("VirtualTimeExecutor: extreme delay and advancement saturate without overflow") {
    VirtualTimeExecutor executor;
    executor.advance_by(1ms);
    bool called = false;
    executor.post_after(VirtualTimeExecutor::duration::max(), [&] { called = true; });
    CHECK(executor.advance_by(1ms) == 0);
    CHECK_FALSE(called);
    CHECK(executor.advance_by(VirtualTimeExecutor::duration::max()) == 1);
    CHECK(called);
    CHECK(executor.now() == VirtualTimeExecutor::duration::max());
    CHECK(executor.advance_by(1ms) == 0);
    CHECK(executor.now() == VirtualTimeExecutor::duration::max());
}

TEST_CASE("VirtualTimeExecutor: reentrant advancement never rolls time back") {
    VirtualTimeExecutor executor;
    executor.post_after(1ms, [&] { executor.advance_to(100ms); });
    CHECK(executor.advance_to(10ms) == 1);
    CHECK(executor.now() == 100ms);
}

namespace {
struct VirtualThrowingCopy {
    std::shared_ptr<bool> fail;
    int* called;
    VirtualThrowingCopy(std::shared_ptr<bool> should_fail, int& count)
        : fail(std::move(should_fail)), called(&count) {}
    VirtualThrowingCopy(const VirtualThrowingCopy& other)
        : fail(other.fail), called(other.called) {
        if (*fail) { throw std::runtime_error("unexpected callback copy"); }
    }
    ~VirtualThrowingCopy() = default;
    VirtualThrowingCopy& operator=(const VirtualThrowingCopy&) = delete;
    VirtualThrowingCopy(VirtualThrowingCopy&&) noexcept = default;
    VirtualThrowingCopy& operator=(VirtualThrowingCopy&&) = delete;
    void operator()() const { ++*called; }
};
}

// Doctest macro internals introduce the flagged branches/type traits.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("VirtualTimeExecutor: consuming a queued callback never copies user captures") {
    VirtualTimeExecutor executor;
    auto fail = std::make_shared<bool>(false);
    int called = 0;
    executor.post(VirtualThrowingCopy{fail, called});
    *fail = true;
    SUBCASE("advance") { CHECK_NOTHROW(executor.advance_by(1ms)); }
    SUBCASE("run until idle") { CHECK_NOTHROW(executor.run_until_idle()); }
    CHECK(called == 1);
    CHECK(executor.pending() == 0);
}
