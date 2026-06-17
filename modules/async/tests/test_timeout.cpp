#include <doctest/doctest.h>

#include "aria/async/timeout.hpp"
#include "aria/async/retry.hpp"
#include "aria/async/virtual_time_executor.hpp"
#include "aria/async/task.hpp"
#include "aria/async/cancellation.hpp"

#include <stdexcept>

using namespace aria::async;
using namespace std::chrono_literals;

// ── Free-function coroutine bodies (avoid lambda-capture coroutine pitfall) ──
namespace {

// Cooperative inner coroutine: waits `delay` virtual time, returns 42.
// Probes the cancellation token at safe points so timeout can unwind.
Task<int> coro_wait_then_42(VirtualTimeExecutor& vt,
                            std::chrono::milliseconds delay,
                            CancellationToken tok) {
    co_await schedule_after(vt, delay);
    tok.throw_if_cancelled();
    co_return 42;
}

// Inner coroutine that ignores its CancellationToken — uses the no-token form.
Task<int> coro_wait_then_99(VirtualTimeExecutor& vt,
                            std::chrono::milliseconds delay) {
    co_await schedule_after(vt, delay);
    co_return 99;
}

Task<void> coro_wait_void(VirtualTimeExecutor& vt,
                          std::chrono::milliseconds delay,
                          CancellationToken tok) {
    co_await schedule_after(vt, delay);
    tok.throw_if_cancelled();
}

Task<int> coro_throws_immediately(CancellationToken /*tok*/) {
    throw std::runtime_error("inner failure");
    co_return 0;  // unreachable
}

// Driver that wraps with_timeout and stores outcome.
struct Outcome {
    enum class Kind { Pending, Value, Timeout, Cancelled, Other };
    Kind kind = Kind::Pending;
    int value = 0;
    std::string err;
};

Task<void> drive_with_timeout(VirtualTimeExecutor& vt,
                              std::chrono::milliseconds deadline,
                              std::chrono::milliseconds inner_delay,
                              Outcome& out) {
    try {
        int v = co_await with_timeout(vt, deadline,
            [&vt, inner_delay](CancellationToken tok) {
                return coro_wait_then_42(vt, inner_delay, tok);
            });
        out.kind = Outcome::Kind::Value;
        out.value = v;
    } catch (const TimeoutError&) {
        out.kind = Outcome::Kind::Timeout;
    } catch (const OperationCancelled&) {
        out.kind = Outcome::Kind::Cancelled;
    } catch (const std::exception& e) {
        out.kind = Outcome::Kind::Other;
        out.err = e.what();
    }
}

Task<void> drive_with_timeout_void(VirtualTimeExecutor& vt,
                                   std::chrono::milliseconds deadline,
                                   std::chrono::milliseconds inner_delay,
                                   Outcome& out) {
    try {
        co_await with_timeout(vt, deadline,
            [&vt, inner_delay](CancellationToken tok) {
                return coro_wait_void(vt, inner_delay, tok);
            });
        out.kind = Outcome::Kind::Value;
    } catch (const TimeoutError&) {
        out.kind = Outcome::Kind::Timeout;
    } catch (const OperationCancelled&) {
        out.kind = Outcome::Kind::Cancelled;
    } catch (const std::exception& e) {
        out.kind = Outcome::Kind::Other;
        out.err = e.what();
    }
}

Task<void> drive_with_timeout_no_token(VirtualTimeExecutor& vt,
                                       std::chrono::milliseconds deadline,
                                       std::chrono::milliseconds inner_delay,
                                       Outcome& out) {
    try {
        int v = co_await with_timeout(vt, deadline,
            [&vt, inner_delay] {
                return coro_wait_then_99(vt, inner_delay);
            });
        out.kind = Outcome::Kind::Value;
        out.value = v;
    } catch (const TimeoutError&) {
        out.kind = Outcome::Kind::Timeout;
    } catch (const std::exception& e) {
        out.kind = Outcome::Kind::Other;
        out.err = e.what();
    }
}

Task<void> drive_with_timeout_parent(VirtualTimeExecutor& vt,
                                     CancellationToken parent,
                                     std::chrono::milliseconds deadline,
                                     std::chrono::milliseconds inner_delay,
                                     Outcome& out) {
    try {
        int v = co_await with_timeout(parent, vt, deadline,
            [&vt, inner_delay](CancellationToken tok) {
                return coro_wait_then_42(vt, inner_delay, tok);
            });
        out.kind = Outcome::Kind::Value;
        out.value = v;
    } catch (const TimeoutError&) {
        out.kind = Outcome::Kind::Timeout;
    } catch (const OperationCancelled&) {
        out.kind = Outcome::Kind::Cancelled;
    } catch (const std::exception& e) {
        out.kind = Outcome::Kind::Other;
        out.err = e.what();
    }
}

Task<void> drive_with_timeout_throws(VirtualTimeExecutor& vt, Outcome& out) {
    try {
        int v = co_await with_timeout(vt, 1000ms,
            [](CancellationToken tok) {
                return coro_throws_immediately(tok);
            });
        out.kind = Outcome::Kind::Value;
        out.value = v;
    } catch (const TimeoutError&) {
        out.kind = Outcome::Kind::Timeout;
    } catch (const std::exception& e) {
        out.kind = Outcome::Kind::Other;
        out.err = e.what();
    }
}

// retry × timeout composition: each attempt has its own timeout
Task<int> drive_retry_of_timeout(VirtualTimeExecutor& vt,
                                 int& attempts,
                                 std::chrono::milliseconds per_attempt,
                                 std::chrono::milliseconds delay_per_call,
                                 int succeed_at) {
    co_return co_await retry(5, [&]() {
        return with_timeout(vt, per_attempt,
            [&vt, &attempts, delay_per_call, succeed_at](CancellationToken tok) -> Task<int> {
                int my_attempt = ++attempts;
                co_await schedule_after(vt, delay_per_call);
                tok.throw_if_cancelled();
                if (my_attempt < succeed_at) {
                    throw std::runtime_error("transient");
                }
                co_return 7;
            });
    });
}

Task<void> drive_retry_of_timeout_outcome(VirtualTimeExecutor& vt,
                                          int& attempts,
                                          std::chrono::milliseconds per_attempt,
                                          std::chrono::milliseconds delay_per_call,
                                          int succeed_at,
                                          Outcome& out) {
    try {
        int v = co_await drive_retry_of_timeout(
            vt, attempts, per_attempt, delay_per_call, succeed_at);
        out.kind = Outcome::Kind::Value;
        out.value = v;
    } catch (const TimeoutError&) {
        out.kind = Outcome::Kind::Timeout;
    } catch (const std::exception& e) {
        out.kind = Outcome::Kind::Other;
        out.err = e.what();
    }
}

}  // namespace

// ── Cases ────────────────────────────────────────────────────────────────────

TEST_CASE("with_timeout: inner finishes before deadline → returns value") {
    VirtualTimeExecutor vt;
    Outcome out;
    auto t = drive_with_timeout(vt, /*deadline=*/500ms, /*inner=*/100ms, out);
    t.start();
    vt.advance_by(100ms);
    CHECK(out.kind == Outcome::Kind::Value);
    CHECK(out.value == 42);
}

TEST_CASE("with_timeout: inner exceeds deadline → TimeoutError") {
    VirtualTimeExecutor vt;
    Outcome out;
    auto t = drive_with_timeout(vt, /*deadline=*/100ms, /*inner=*/500ms, out);
    t.start();
    vt.advance_by(99ms);
    CHECK(out.kind == Outcome::Kind::Pending);
    vt.advance_by(1ms);  // deadline fires; inner_tok flips
    // Inner is still parked on its 500ms schedule_after — advance to its
    // scheduled time so it resumes, probes the (now-cancelled) token, and
    // throws OperationCancelled, which `with_timeout` translates to TimeoutError.
    vt.advance_by(400ms);
    CHECK(out.kind == Outcome::Kind::Timeout);
}

TEST_CASE("with_timeout: void return path") {
    VirtualTimeExecutor vt;
    Outcome out;
    auto t = drive_with_timeout_void(vt, 500ms, 100ms, out);
    t.start();
    vt.advance_by(100ms);
    CHECK(out.kind == Outcome::Kind::Value);
}

TEST_CASE("with_timeout: void + deadline expires → TimeoutError") {
    VirtualTimeExecutor vt;
    Outcome out;
    auto t = drive_with_timeout_void(vt, 50ms, 200ms, out);
    t.start();
    vt.advance_by(50ms);   // timer fires, cancels inner_tok
    vt.advance_by(150ms);  // inner resumes, throws cancelled → TimeoutError
    CHECK(out.kind == Outcome::Kind::Timeout);
}

TEST_CASE("with_timeout: no-token factory still observes timeout via TimeoutError") {
    // Without a token the inner work cannot cooperatively cancel — but the
    // timer still fires, and once inner naturally completes (or, in this
    // case, times out before completing), the wrapper observes the race.
    // In cooperative-only mode, no-token factories that DON'T
    // complete before the deadline never resume the wrapper. We test the
    // happy path here (inner completes first).
    VirtualTimeExecutor vt;
    Outcome out;
    auto t = drive_with_timeout_no_token(vt, /*deadline=*/500ms, /*inner=*/100ms, out);
    t.start();
    vt.advance_by(100ms);
    CHECK(out.kind == Outcome::Kind::Value);
    CHECK(out.value == 99);
}

TEST_CASE("with_timeout: inner throws non-cancel exception → propagates verbatim") {
    VirtualTimeExecutor vt;
    Outcome out;
    auto t = drive_with_timeout_throws(vt, out);
    t.start();
    CHECK(out.kind == Outcome::Kind::Other);
    CHECK(out.err == "inner failure");
}

TEST_CASE("with_timeout: parent-cancel beats timeout (priority test)") {
    VirtualTimeExecutor vt;
    CancellationSource parent_src;
    Outcome out;
    auto t = drive_with_timeout_parent(vt, parent_src.token(),
                                       /*deadline=*/1000ms,
                                       /*inner=*/500ms, out);
    t.start();
    vt.advance_by(100ms);
    parent_src.cancel();        // parent fires before deadline
    vt.advance_by(400ms);       // inner resumes from its schedule_after, sees cancel
    CHECK(out.kind == Outcome::Kind::Cancelled);  // NOT Timeout
}

TEST_CASE("with_timeout: parent-cancel after deadline → still Cancelled") {
    // If both fire, parent always wins.
    VirtualTimeExecutor vt;
    CancellationSource parent_src;
    Outcome out;
    auto t = drive_with_timeout_parent(vt, parent_src.token(),
                                       /*deadline=*/100ms,
                                       /*inner=*/500ms, out);
    t.start();
    vt.advance_by(100ms);   // deadline fires first; inner_tok cancelled by timer
    parent_src.cancel();    // parent also cancels (post-deadline)
    vt.advance_by(400ms);
    // Parent is cancelled when wrapper unwinds → wrapper throws OperationCancelled.
    CHECK(out.kind == Outcome::Kind::Cancelled);
}

TEST_CASE("retry × with_timeout: each attempt gets its own deadline") {
    // Each attempt times out at 100ms. Inner work needs 80ms but throws on
    // attempts < 3 (by raising "transient"). With 5-retry budget and 80ms <
    // 100ms per-attempt, we should succeed on attempt 3.
    VirtualTimeExecutor vt;
    int attempts = 0;
    Outcome out;
    auto t = drive_retry_of_timeout_outcome(vt,
                                            attempts,
                                            /*per_attempt=*/100ms,
                                            /*delay_per_call=*/80ms,
                                            /*succeed_at=*/3,
                                            out);
    t.start();
    // Drive enough virtual time to cover 3 attempts of 80ms each.
    vt.advance_by(80ms);   // attempt 1: throws "transient"
    vt.advance_by(80ms);   // attempt 2: throws "transient"
    vt.advance_by(80ms);   // attempt 3: succeeds
    CHECK(attempts == 3);
    CHECK(out.kind == Outcome::Kind::Value);
    CHECK(out.value == 7);
}

TEST_CASE("retry × with_timeout: each attempt times out independently") {
    // Per-attempt deadline 100ms; inner takes 200ms (always > deadline).
    // Every attempt times out → eventually retry exhausts.
    VirtualTimeExecutor vt;
    int attempts = 0;
    Outcome out;
    auto t = drive_retry_of_timeout_outcome(vt,
                                            attempts,
                                            /*per_attempt=*/100ms,
                                            /*delay_per_call=*/200ms,
                                            /*succeed_at=*/100,  // never succeed
                                            out);
    t.start();
    // Each attempt: 100ms deadline → flip token → inner resumes at 200ms,
    // throws cancelled → TimeoutError → retry catches & re-attempts.
    // Retry budget is 5; need 5*200ms = 1000ms to exhaust.
    vt.advance_by(1000ms);
    CHECK(attempts == 5);
    CHECK(out.kind == Outcome::Kind::Timeout);
}

// ── OnTimeout::Fail (fail-fast) cases ────────────────────────────────────

namespace {

Task<void> drive_with_timeout_fail(VirtualTimeExecutor& vt,
                                   std::chrono::milliseconds deadline,
                                   std::chrono::milliseconds inner_delay,
                                   Outcome& out) {
    try {
        int v = co_await with_timeout(vt, deadline,
            [&vt, inner_delay](CancellationToken tok) {
                return coro_wait_then_42(vt, inner_delay, tok);
            },
            OnTimeout::Fail);
        out.kind = Outcome::Kind::Value;
        out.value = v;
    } catch (const TimeoutError&) {
        out.kind = Outcome::Kind::Timeout;
    } catch (const OperationCancelled&) {
        out.kind = Outcome::Kind::Cancelled;
    } catch (const std::exception& e) {
        out.kind = Outcome::Kind::Other;
        out.err = e.what();
    }
}

Task<void> drive_with_timeout_fail_void(VirtualTimeExecutor& vt,
                                        std::chrono::milliseconds deadline,
                                        std::chrono::milliseconds inner_delay,
                                        Outcome& out) {
    try {
        co_await with_timeout(vt, deadline,
            [&vt, inner_delay](CancellationToken tok) {
                return coro_wait_void(vt, inner_delay, tok);
            },
            OnTimeout::Fail);
        out.kind = Outcome::Kind::Value;
    } catch (const TimeoutError&) {
        out.kind = Outcome::Kind::Timeout;
    } catch (const OperationCancelled&) {
        out.kind = Outcome::Kind::Cancelled;
    } catch (const std::exception& e) {
        out.kind = Outcome::Kind::Other;
        out.err = e.what();
    }
}

Task<void> drive_with_timeout_fail_no_token(VirtualTimeExecutor& vt,
                                            std::chrono::milliseconds deadline,
                                            std::chrono::milliseconds inner_delay,
                                            Outcome& out) {
    try {
        int v = co_await with_timeout(vt, deadline,
            [&vt, inner_delay] {
                return coro_wait_then_99(vt, inner_delay);
            },
            OnTimeout::Fail);
        out.kind = Outcome::Kind::Value;
        out.value = v;
    } catch (const TimeoutError&) {
        out.kind = Outcome::Kind::Timeout;
    } catch (const std::exception& e) {
        out.kind = Outcome::Kind::Other;
        out.err = e.what();
    }
}

Task<void> drive_with_timeout_fail_parent(VirtualTimeExecutor& vt,
                                          CancellationToken parent,
                                          std::chrono::milliseconds deadline,
                                          std::chrono::milliseconds inner_delay,
                                          Outcome& out) {
    try {
        int v = co_await with_timeout(parent, vt, deadline,
            [&vt, inner_delay](CancellationToken tok) {
                return coro_wait_then_42(vt, inner_delay, tok);
            },
            OnTimeout::Fail);
        out.kind = Outcome::Kind::Value;
        out.value = v;
    } catch (const TimeoutError&) {
        out.kind = Outcome::Kind::Timeout;
    } catch (const OperationCancelled&) {
        out.kind = Outcome::Kind::Cancelled;
    } catch (const std::exception& e) {
        out.kind = Outcome::Kind::Other;
        out.err = e.what();
    }
}

Task<void> drive_with_timeout_fail_throws(VirtualTimeExecutor& vt, Outcome& out) {
    try {
        int v = co_await with_timeout(vt, 1000ms,
            [](CancellationToken tok) {
                return coro_throws_immediately(tok);
            },
            OnTimeout::Fail);
        out.kind = Outcome::Kind::Value;
        out.value = v;
    } catch (const TimeoutError&) {
        out.kind = Outcome::Kind::Timeout;
    } catch (const std::exception& e) {
        out.kind = Outcome::Kind::Other;
        out.err = e.what();
    }
}

}  // namespace

TEST_CASE("with_timeout::Fail: inner finishes before deadline → returns value") {
    VirtualTimeExecutor vt;
    Outcome out;
    auto t = drive_with_timeout_fail(vt, /*deadline=*/500ms, /*inner=*/100ms, out);
    t.start();
    vt.advance_by(100ms);
    CHECK(out.kind == Outcome::Kind::Value);
    CHECK(out.value == 42);
}

TEST_CASE("with_timeout::Fail: deadline expires → parent resumes IMMEDIATELY") {
    // Key fail-fast property: the parent does not have to wait for inner
    // to unwind. Even though inner is parked on a 500ms schedule_after,
    // the parent observes TimeoutError as soon as the 100ms deadline
    // fires, BEFORE inner reaches its natural completion point.
    VirtualTimeExecutor vt;
    Outcome out;
    auto t = drive_with_timeout_fail(vt, /*deadline=*/100ms, /*inner=*/500ms, out);
    t.start();
    vt.advance_by(99ms);
    CHECK(out.kind == Outcome::Kind::Pending);
    vt.advance_by(1ms);  // deadline fires NOW; parent must already be Timeout.
    CHECK(out.kind == Outcome::Kind::Timeout);
    // Inner is still parked; advancing further must NOT change the outcome
    // (its eventual completion is silently discarded).
    vt.advance_by(1000ms);
    CHECK(out.kind == Outcome::Kind::Timeout);
}

TEST_CASE("with_timeout::Fail: void return path") {
    VirtualTimeExecutor vt;
    Outcome out;
    auto t = drive_with_timeout_fail_void(vt, 500ms, 100ms, out);
    t.start();
    vt.advance_by(100ms);
    CHECK(out.kind == Outcome::Kind::Value);
}

TEST_CASE("with_timeout::Fail: void + deadline expires → fast TimeoutError") {
    VirtualTimeExecutor vt;
    Outcome out;
    auto t = drive_with_timeout_fail_void(vt, 50ms, 200ms, out);
    t.start();
    vt.advance_by(50ms);   // timer fires
    CHECK(out.kind == Outcome::Kind::Timeout);  // parent already resumed
    vt.advance_by(1000ms); // inner naturally completes; outcome unchanged
    CHECK(out.kind == Outcome::Kind::Timeout);
}

TEST_CASE("with_timeout::Fail: no-token factory + deadline → fast TimeoutError") {
    // Without a token the inner CANNOT cooperatively cancel, but Fail
    // mode does not require it: the parent unblocks regardless.
    VirtualTimeExecutor vt;
    Outcome out;
    auto t = drive_with_timeout_fail_no_token(vt, /*deadline=*/100ms, /*inner=*/500ms, out);
    t.start();
    vt.advance_by(100ms);
    CHECK(out.kind == Outcome::Kind::Timeout);
}

TEST_CASE("with_timeout::Fail: inner throws non-cancel exception → propagates verbatim") {
    VirtualTimeExecutor vt;
    Outcome out;
    auto t = drive_with_timeout_fail_throws(vt, out);
    t.start();
    CHECK(out.kind == Outcome::Kind::Other);
    CHECK(out.err == "inner failure");
}

TEST_CASE("with_timeout::Fail: parent-cancel beats deadline (priority)") {
    VirtualTimeExecutor vt;
    CancellationSource parent_src;
    Outcome out;
    auto t = drive_with_timeout_fail_parent(vt, parent_src.token(),
                                            /*deadline=*/1000ms,
                                            /*inner=*/500ms, out);
    t.start();
    vt.advance_by(100ms);
    parent_src.cancel();  // parent fires before deadline
    CHECK(out.kind == Outcome::Kind::Cancelled);  // immediate, not Timeout
}

TEST_CASE("with_timeout::Fail: inner that completed first wins over later timer") {
    // Inner finishes at 50ms, deadline is 100ms. The deadline still
    // fires (timer is already armed) but its CAS loses; the timer's
    // post-fire observation must NOT corrupt the already-resolved
    // result.
    VirtualTimeExecutor vt;
    Outcome out;
    auto t = drive_with_timeout_fail(vt, /*deadline=*/100ms, /*inner=*/50ms, out);
    t.start();
    vt.advance_by(50ms);  // inner completes, parent resumes with value
    CHECK(out.kind == Outcome::Kind::Value);
    CHECK(out.value == 42);
    vt.advance_by(100ms); // timer fires post-hoc; outcome must not flip
    CHECK(out.kind == Outcome::Kind::Value);
    CHECK(out.value == 42);
}

TEST_CASE("with_timeout::Fail: inner already done at await_suspend (sync race)") {
    // Edge case: if the inner factory returns a Task that completes
    // synchronously (no schedule_after), the inner driver wins the
    // race BEFORE await_suspend stores the parent handle. The awaiter
    // must detect this and skip suspension — manifested here as the
    // outcome being Value immediately upon t.start() without any
    // virtual-time advance.
    struct Sync {
        static Task<int> factory(CancellationToken) { co_return 7; }
    };
    static auto driver = [](VirtualTimeExecutor& vt, Outcome& out) -> Task<void> {
        try {
            int v = co_await with_timeout(vt, 100ms, &Sync::factory, OnTimeout::Fail);
            out.kind = Outcome::Kind::Value;
            out.value = v;
        } catch (...) {
            out.kind = Outcome::Kind::Other;
        }
    };
    VirtualTimeExecutor vt;
    Outcome out;
    auto t = driver(vt, out);
    t.start();
    // Without any vt.advance_by, the synchronous inner must have
    // populated the result and parent must already have observed it.
    CHECK(out.kind == Outcome::Kind::Value);
    CHECK(out.value == 7);
}
