#include <doctest/doctest.h>

#include "aria/async/retry.hpp"
#include "aria/async/cancellation.hpp"
#include "aria/async/task.hpp"
#include "aria/async/virtual_time_executor.hpp"

#include <chrono>
#include <exception>
#include <stdexcept>
#include <string>

using namespace aria::async;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
//  Free functions rather than lambdas: a coroutine defined in a lambda body
//  reads its captures through the lambda's `this`, which does not survive
//  into the coroutine frame. The rest of the async suite follows the same
//  convention (see test_timeout.cpp).
// ---------------------------------------------------------------------------
namespace {

struct Outcome {
    enum class Kind { None, Value, Cancelled, Other } kind = Kind::None;
    int value = 0;
    std::string err;
};

// Always throws a retryable domain error; counts how many times it ran.
Task<int> always_fails(int& attempts) {
    ++attempts;
    throw std::runtime_error("network: transient");
    co_return 0;  // unreachable, keeps this a coroutine
}

// Fails the first `fail_times` attempts, then succeeds with 42.
Task<int> fails_then_succeeds(int& attempts, int fail_times) {
    ++attempts;
    if (attempts <= fail_times) {
        throw std::runtime_error("network: transient");
    }
    co_return 42;
}

// Throws OperationCancelled, exactly as a cancelled operation would.
Task<int> cancelled_op(int& attempts) {
    ++attempts;
    throw OperationCancelled{};
    co_return 0;  // unreachable
}

// Observes a real token the way user code is expected to.
Task<int> op_honouring_token(int& attempts, CancellationToken tok) {
    ++attempts;
    tok.throw_if_cancelled();
    co_return 7;
}

// Drives any `Task<int>` and classifies how it finished. Keeping the
// try/catch in a plain coroutine (not a lambda) matches the suite's style.
Task<void> drive(Task<int> inner, Outcome& out) {
    try {
        out.value = co_await std::move(inner);
        out.kind  = Outcome::Kind::Value;
    } catch (const OperationCancelled&) {
        out.kind = Outcome::Kind::Cancelled;
    } catch (const std::exception& e) {
        out.kind = Outcome::Kind::Other;
        out.err  = e.what();
    }
}

}  // namespace

// ── Baseline retry behaviour ─────────────────────────────────────────────────

TEST_CASE("retry: retries a failing factory up to max_attempts") {
    VirtualTimeExecutor vt;
    int     attempts = 0;
    Outcome out;

    auto t = drive(retry(3, [&attempts] { return always_fails(attempts); }), out);
    t.start();
    vt.run_until_idle();

    CHECK(out.kind == Outcome::Kind::Other);
    CHECK(attempts == 3);
}

TEST_CASE("retry: stops as soon as the factory succeeds") {
    VirtualTimeExecutor vt;
    int     attempts = 0;
    Outcome out;

    auto t = drive(
        retry(5, [&attempts] { return fails_then_succeeds(attempts, 2); }), out);
    t.start();
    vt.run_until_idle();

    CHECK(out.kind == Outcome::Kind::Value);
    CHECK(out.value == 42);
    CHECK(attempts == 3);  // 2 failures + 1 success, not 5
}

TEST_CASE("retry_if: honours a custom predicate") {
    VirtualTimeExecutor vt;
    int     attempts = 0;
    Outcome out;

    // The predicate rejects this error, so no retry should happen.
    auto t = drive(
        retry_if(
            5,
            [](const std::exception& e) {
                return std::string{e.what()}.starts_with("retryable:");
            },
            [&attempts] { return always_fails(attempts); }),
        out);
    t.start();
    vt.run_until_idle();

    CHECK(out.kind == Outcome::Kind::Other);
    CHECK(attempts == 1);  // predicate said "don't retry"
}

// ---------------------------------------------------------------------------
//  Regression: cancellation must NOT be retried.
//
//  `OperationCancelled` derives from `std::exception`. Before the fix,
//  `retry_impl_` caught it in its generic `catch (const std::exception&)`
//  arm, and because the default `should_retry` returns `true`
//  unconditionally, a cancelled operation was re-run until `max_attempts`
//  was exhausted — the caller's cancellation was silently defeated and any
//  side effect they cancelled happened N times instead of once.
//
//  Correct behaviour: rethrow on the first `OperationCancelled`, so the
//  factory runs exactly once and the cancellation reaches the awaiter.
// ---------------------------------------------------------------------------
TEST_CASE("retry: OperationCancelled is never retried") {
    VirtualTimeExecutor vt;
    int     attempts = 0;
    Outcome out;

    auto t = drive(retry(5, [&attempts] { return cancelled_op(attempts); }), out);
    t.start();
    vt.run_until_idle();

    CHECK(out.kind == Outcome::Kind::Cancelled);
    CHECK(attempts == 1);  // pre-fix: 5
}

TEST_CASE("retry: an already-cancelled token aborts the retry loop") {
    VirtualTimeExecutor vt;
    CancellationSource  src;
    int                 attempts = 0;
    Outcome             out;

    src.cancel();  // cancelled before the first attempt
    auto tok = src.token();

    auto t = drive(
        retry(4, [&attempts, tok] { return op_honouring_token(attempts, tok); }),
        out);
    t.start();
    vt.run_until_idle();

    CHECK(out.kind == Outcome::Kind::Cancelled);
    CHECK(attempts == 1);  // pre-fix: 4
}

TEST_CASE("retry_with_backoff: cancellation is not retried across delays") {
    VirtualTimeExecutor vt;
    int                 attempts = 0;
    Outcome             out;

    auto t = drive(
        retry_with_backoff(5, 10ms, vt,
                           [&attempts] { return cancelled_op(attempts); }),
        out);
    t.start();
    vt.advance_by(10s);  // plenty of virtual time for every backoff step

    CHECK(out.kind == Outcome::Kind::Cancelled);
    CHECK(attempts == 1);  // pre-fix: 5, spread across exponential delays
}

// ---------------------------------------------------------------------------
//  Regression: the backoff delay must not shift past the width of the type.
//
//  The delay used to be `initial * (1 << attempt)` on a plain `int`, which
//  is signed-overflow UB once `attempt` reaches 31 — and `max_attempts` is
//  caller-supplied with no ceiling. The exponent is now clamped (at 20) and
//  the shift performed in `unsigned long long`, so every attempt past the
//  cliff keeps a finite, well-defined delay.
//
//  Virtual time lets us cross that cliff without waiting on a real clock.
//  With a 1ms base the clamped step is 2^20ms ≈ 17.5 min, so advancing a
//  virtual day per iteration clears each delay comfortably.
// ---------------------------------------------------------------------------
TEST_CASE("retry_with_backoff: large attempt counts stay well-defined") {
    VirtualTimeExecutor vt;
    int                 attempts = 0;
    Outcome             out;

    auto t = drive(
        retry_with_backoff(40,  // deliberately past the 31-bit shift cliff
                           1ms, vt,
                           [&attempts] {
                               return fails_then_succeeds(attempts, 35);
                           }),
        out);
    t.start();

    // The clamped backoff is finite, so the loop must terminate. Advance in
    // generous steps until it does.
    for (int i = 0; i < 60 && out.kind == Outcome::Kind::None; ++i) {
        vt.advance_by(std::chrono::hours{24});
        vt.run_until_idle();
    }

    CHECK(out.kind == Outcome::Kind::Value);
    CHECK(out.value == 42);
    CHECK(attempts == 36);
}
