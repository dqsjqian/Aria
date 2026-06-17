// Regression tests for the AsyncCommandResult contract introduced
// alongside `co_execute(...) -> Task<AsyncCommandResult<R>>`.
//
// The legacy `co_execute` returned `Task<R>` and silently collapsed
// "dropped" onto a default-constructed R, leaked OperationCancelled
// out of the awaitable, and forced R to be DefaultConstructible.
// The new contract:
//
//   * NEVER throws (Cancellation / Failure are folded into status)
//   * Distinguishes Completed / Dropped / Cancelled / Failed
//   * Works with non-default-constructible R
//   * `r.value` is engaged iff `r.completed()`
//   * `r.error` is engaged iff `Failed` or `Cancelled`
//
// Each TEST_CASE below pins one quadrant of the 4 statuses x {R, void}
// matrix, plus one structural test for the non-default-constructible R
// case that previously failed to compile.

#include <doctest/doctest.h>

#include "aria/async/async_command.hpp"
#include "aria/async/async_command_result.hpp"
#include "aria/async/executor.hpp"
#include "aria/async/task.hpp"

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

using namespace aria;
using namespace aria::async;

namespace {

// All tests in this file run on InlineExecutor for both ui and worker.
// That keeps the coroutine machinery synchronous: the moment we
// `start_detached_()` the launcher coroutine, it pumps to completion
// inside the call, so we can assert on the captured result without
// any wait_until loop.
//
// (Inline+Inline is the *one* combination AsyncCommand permits where
// both schedule_on hops resolve eagerly. It is the canonical setup
// for unit-level Result contract tests.)
struct InlineEnv {
    InlineExecutor ui;
    InlineExecutor worker;
};

}  // namespace

// ── Quadrant: R != void, status == Completed ─────────────────────────
TEST_CASE("co_execute: completed populates value and clears error") {
    InlineEnv e;
    AsyncCommand<int, int> cmd{e.ui, e.worker,
        [](int x) -> Task<int> { co_return x + 1; }};

    AsyncCommandResult<int> r;
    [&]() -> Task<void> {
        r = co_await cmd.co_execute(40);
    }().start_detached_();

    REQUIRE(r.completed());
    CHECK(static_cast<bool>(r));
    CHECK(*r == 41);
    CHECK_FALSE(r.error.has_value());
}

// ── Quadrant: R == void, status == Completed ─────────────────────────
TEST_CASE("co_execute: void completed has no value field but reports success") {
    InlineEnv e;
    int side = 0;
    AsyncCommand<void> cmd{e.ui, e.worker,
        [&]() -> Task<void> { side = 7; co_return; }};

    AsyncCommandResult<void> r;
    [&]() -> Task<void> {
        r = co_await cmd.co_execute();
    }().start_detached_();

    CHECK(r.completed());
    CHECK(static_cast<bool>(r));
    CHECK(side == 7);
    CHECK_FALSE(r.error.has_value());
}

// ── Quadrant: status == Dropped ──────────────────────────────────────
//
// Drive a DropIfRunning command into the "busy" state with a fire-and-
// forget execute() that stalls on a gate, then issue a co_execute() —
// the policy must reject it without ever entering the action.
TEST_CASE("co_execute: DropIfRunning while busy yields Dropped (R != void)") {
    // We need a worker that actually runs concurrently for the busy
    // condition to be observable; InlineExecutor would short-circuit
    // and drain the first invocation before the second one is issued.
    MainThreadExecutor ui;
    ThreadPoolExecutor worker{1};

    std::atomic<bool> gate{false};
    std::atomic<int>  entered{0};

    AsyncCommand<int, int> cmd{ui, worker,
        [&](CancellationToken, int x) -> Task<int> {
            entered.fetch_add(1, std::memory_order_acq_rel);
            while (!gate.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            co_return x;
        },
        AsyncCommandPolicy::DropIfRunning};

    // First invocation — fire-and-forget, parks on the gate.
    cmd.execute(1);

    // Wait until it has actually crossed into the body so policy
    // really sees inflight > 0 when we call co_execute.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (entered.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        ui.drain();
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    REQUIRE(entered.load() == 1);

    // Second invocation — co_execute. The policy must drop it before
    // the action runs.
    AsyncCommandResult<int> r;
    bool launcher_done = false;
    [&]() -> Task<void> {
        r = co_await cmd.co_execute(2);
        launcher_done = true;
    }().start_detached_();

    // Drop path is synchronous from the caller's perspective: no hop
    // needed, accept_new_invocation_ returns false and we co_return
    // immediately. Pump the ui queue once to release the launcher.
    ui.drain();
    REQUIRE(launcher_done);

    CHECK(r.dropped());
    CHECK_FALSE(static_cast<bool>(r));
    CHECK_FALSE(r.value.has_value());
    CHECK_FALSE(r.error.has_value());
    CHECK(entered.load() == 1);  // second action body never ran

    // Release the first invocation so cmd's destructor doesn't
    // leave a thread blocked on the gate.
    gate = true;
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (cmd.is_executing.get() && std::chrono::steady_clock::now() < deadline) {
        ui.drain();
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
}

// ── Quadrant: status == Cancelled ────────────────────────────────────
TEST_CASE("co_execute: Cancellation inside the body yields Cancelled, not throw") {
    InlineEnv e;
    AsyncCommand<int, int> cmd{e.ui, e.worker,
        [](CancellationToken, int) -> Task<int> {
            throw OperationCancelled{};
            co_return 0;  // unreachable, silences compiler
        }};

    AsyncCommandResult<int> r;
    [&]() -> Task<void> {
        r = co_await cmd.co_execute(0);
    }().start_detached_();

    CHECK(r.cancelled());
    CHECK_FALSE(static_cast<bool>(r));
    CHECK_FALSE(r.value.has_value());
    REQUIRE(r.error.has_value());
    CHECK(r.error->kind == ErrorKind::Cancellation);
    // Cancellation is not an observable failure.
    CHECK_FALSE(cmd.last_error.get().has_value());
}

// ── Quadrant: status == Failed ───────────────────────────────────────
TEST_CASE("co_execute: action throwing a domain exception yields Failed") {
    InlineEnv e;
    AsyncCommand<int, int> cmd{e.ui, e.worker,
        [](int) -> Task<int> {
            throw std::runtime_error("kaboom");
            co_return 0;
        }};

    AsyncCommandResult<int> r;
    [&]() -> Task<void> {
        r = co_await cmd.co_execute(0);
    }().start_detached_();

    CHECK(r.failed());
    CHECK_FALSE(static_cast<bool>(r));
    CHECK_FALSE(r.value.has_value());
    REQUIRE(r.error.has_value());
    CHECK(r.error->message == "kaboom");
    // last_error is also populated for awaitable failures, matching
    // the fire-and-forget observable surface.
    REQUIRE(cmd.last_error.get().has_value());
    CHECK(cmd.last_error.get()->message == "kaboom");
}

TEST_CASE("co_execute: void Failed branch") {
    InlineEnv e;
    AsyncCommand<void> cmd{e.ui, e.worker,
        []() -> Task<void> {
            throw std::runtime_error("v-kaboom");
            co_return;
        }};

    AsyncCommandResult<void> r;
    [&]() -> Task<void> {
        r = co_await cmd.co_execute();
    }().start_detached_();

    CHECK(r.failed());
    REQUIRE(r.error.has_value());
    CHECK(r.error->message == "v-kaboom");
}

// ── Structural: non-default-constructible R compiles & runs ──────────
//
// The legacy `co_return R{}` drop path required R to be
// DefaultConstructible. The new contract lifts that constraint —
// `AsyncCommandResult<R>::dropped_()` produces an empty `optional<R>`
// without calling R's default ctor.
namespace {
struct NonDefault {
    int payload;
    explicit NonDefault(int v) : payload(v) {}
    NonDefault() = delete;
    NonDefault(const NonDefault&) = default;
    NonDefault(NonDefault&&) noexcept = default;
    NonDefault& operator=(const NonDefault&) = default;
    NonDefault& operator=(NonDefault&&) noexcept = default;
    // Equality is required by Property<optional<R>>, but
    // default-constructibility (the constraint #2 lifted) is NOT.
    friend bool operator==(const NonDefault& a, const NonDefault& b) noexcept {
        return a.payload == b.payload;
    }
};
}  // namespace

TEST_CASE("AsyncCommand<NonDefault, ...>::co_execute compiles and runs") {
    InlineEnv e;
    AsyncCommand<NonDefault, int> cmd{e.ui, e.worker,
        [](int x) -> Task<NonDefault> { co_return NonDefault{x}; }};

    AsyncCommandResult<NonDefault> r;
    [&]() -> Task<void> {
        r = co_await cmd.co_execute(123);
    }().start_detached_();

    REQUIRE(r.completed());
    CHECK(r->payload == 123);
}

// ── Convenience: value_or fallback on non-completed results ─────────
TEST_CASE("AsyncCommandResult::value_or returns fallback on non-completed") {
    auto failed = AsyncCommandResult<int>::failed_(
        ::aria::Error::from_exception(std::make_exception_ptr(
            std::runtime_error("x")), "test"));
    CHECK(failed.value_or(99) == 99);

    auto ok = AsyncCommandResult<int>::completed_with(7);
    CHECK(ok.value_or(99) == 7);
}
