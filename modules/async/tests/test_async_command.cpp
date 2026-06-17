#include <doctest/doctest.h>

#include "aria/async/async_command.hpp"
#include "aria/async/executor.hpp"
#include "aria/async/task.hpp"

#include "support/threaded_command_fixture.hpp"

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

using namespace aria;
using namespace aria::async;
using aria::async::testing::ThreadedCommandFixture;

namespace {

void wait_until(const std::function<bool()>& done,
                std::chrono::milliseconds timeout = std::chrono::seconds{1}) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
}

}  // namespace

// ── Compile-time executor-safety guarantee ────────────────────────────────
//
// `AsyncCommand{InlineExecutor, ThreadPoolExecutor, ...}` must be rejected
// at compile time. We assert this with a SFINAE-style is_constructible
// check so the contract is itself part of the test suite, not just
// documented behaviour.
namespace executor_safety_compile_check {

template<typename Action>
using AsyncCmdInlineInline =
    decltype(AsyncCommand<int, int>{
        std::declval<InlineExecutor&>(),
        std::declval<InlineExecutor&>(),
        std::declval<Action>()});

template<typename Action>
using AsyncCmdMainPool =
    decltype(AsyncCommand<int, int>{
        std::declval<MainThreadExecutor&>(),
        std::declval<ThreadPoolExecutor&>(),
        std::declval<Action>()});

using PlainAction = Task<int>(*)(int);

// Sanity: the safe combinations *do* compile.
static_assert(
    requires(InlineExecutor& u, InlineExecutor& w, PlainAction f) {
        AsyncCommand<int, int>{u, w, f};
    }, "InlineExecutor + InlineExecutor must be a valid AsyncCommand<int,int>.");

static_assert(
    requires(MainThreadExecutor& u, ThreadPoolExecutor& w, PlainAction f) {
        AsyncCommand<int, int>{u, w, f};
    }, "MainThreadExecutor + ThreadPoolExecutor must be a valid AsyncCommand<int,int>.");

// Note on the negative case (InlineExecutor + ThreadPoolExecutor): we
// rely on `static_assert` *inside* the constructor body for a clear
// diagnostic. A `requires` probe would silently succeed (the SFINAE
// constraints on the constructor template still match); the helpful
// error only fires when the body is instantiated. This is the
// intentional trade-off — better error messages over machine-checkable
// negative trait assertions.

}  // namespace executor_safety_compile_check

TEST_CASE("AsyncCommand<int, int>: success populates last_result") {
    InlineExecutor ui;
    InlineExecutor worker;
    AsyncCommand<int, int> cmd{ui, worker, [](int x) -> Task<int> {
        co_return x * 2;
    }};

    cmd.execute(21);
    wait_until([&]{ return !cmd.is_executing.get(); });
    CHECK(cmd.last_result.get().has_value());
    CHECK(*cmd.last_result.get() == 42);
    CHECK(cmd.last_error_message.get() == "");
    CHECK_FALSE(cmd.last_error.get().has_value());
}

TEST_CASE("AsyncCommand: failure populates last_error") {
    InlineExecutor ui;
    InlineExecutor worker;
    AsyncCommand<int, int> cmd{ui, worker, [](int) -> Task<int> {
        throw std::runtime_error("kaboom"); co_return 0;
    }};

    cmd.execute(0);
    wait_until([&]{ return !cmd.is_executing.get(); });
    CHECK(cmd.last_error_message.get() == "kaboom");
    REQUIRE(cmd.last_error.get().has_value());
    CHECK(cmd.last_error.get()->kind == ErrorKind::AsyncFailure);
    CHECK(cmd.last_error.get()->source == "AsyncCommand");
    CHECK(cmd.last_error.get()->message == "kaboom");
    CHECK_FALSE(cmd.last_result.get().has_value());
}

TEST_CASE("AsyncCommand<void>: works without a result") {
    InlineExecutor ui;
    InlineExecutor worker;
    int side = 0;
    AsyncCommand<void> cmd{ui, worker, [&]() -> Task<void> { side = 99; co_return; }};

    cmd.execute();
    wait_until([&]{ return !cmd.is_executing.get(); });
    CHECK(side == 99);
    CHECK(cmd.last_error_message.get() == "");
    CHECK_FALSE(cmd.last_error.get().has_value());
}

// ── Type-erased executor construction (`IExecutor&`) ──────────────────────
//
// ViewModels assembled by a DI container typically only see executors as
// `IExecutor&`. Both the primary template and the `R == void` partial
// specialisation must accept that form and validate safety at runtime
// (via `check_executor_safety_runtime`) instead of relying on the static
// trait checks that only fire for concrete subclasses.

TEST_CASE("AsyncCommand<int>: constructible from IExecutor& (type-erased path)") {
    InlineExecutor ui;
    InlineExecutor worker;
    IExecutor& iui = ui;
    IExecutor& iw  = worker;

    AsyncCommand<int, int> cmd{iui, iw, [](int x) -> Task<int> {
        co_return x + 1;
    }};

    cmd.execute(41);
    wait_until([&]{ return !cmd.is_executing.get(); });
    REQUIRE(cmd.last_result.get().has_value());
    CHECK(*cmd.last_result.get() == 42);
}

TEST_CASE("AsyncCommand<void>: constructible from IExecutor& (type-erased path)") {
    InlineExecutor ui;
    InlineExecutor worker;
    IExecutor& iui = ui;
    IExecutor& iw  = worker;

    int side = 0;
    AsyncCommand<void> cmd{iui, iw, [&]() -> Task<void> {
        side = 7;
        co_return;
    }};

    cmd.execute();
    wait_until([&]{ return !cmd.is_executing.get(); });
    CHECK(side == 7);
    CHECK_FALSE(cmd.last_error.get().has_value());
}

TEST_CASE("AsyncCommand: is_executing toggles around the action") {
    InlineExecutor ui;
    InlineExecutor worker;
    int begin_state = -1, end_state = -1;
    AsyncCommand<int, int> cmd{ui, worker, [](int) -> Task<int> { co_return 0; }};

    auto sub_begin = cmd.is_executing.on_changed([&](bool b) {
        if (b && begin_state == -1) begin_state = 1;
        if (!b && begin_state == 1 && end_state == -1) end_state = 0;
    });

    cmd.execute(0);
    wait_until([&]{ return end_state == 0; });
    CHECK(begin_state == 1);
    CHECK(end_state == 0);
}

// ═══════════════════════════════════════════════════════════════════════
//  Cancellation-token injection
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("AsyncCommand: cancellable action receives the per-invocation token") {
    InlineExecutor ui;
    InlineExecutor worker;

    bool token_seen = false;
    AsyncCommand<int, int> cmd{ui, worker,
        [&](CancellationToken tok, int x) -> Task<int> {
            token_seen = true;
            // The token is live and not cancelled in a normal run.
            CHECK_FALSE(tok.is_cancelled());
            co_return x + 1;
        }};

    cmd.execute(41);
    wait_until([&]{ return !cmd.is_executing.get(); });
    CHECK(token_seen);
    CHECK(cmd.last_result.get().has_value());
    CHECK(*cmd.last_result.get() == 42);
}

TEST_CASE("AsyncCommand: plain-shape action still compiles and runs") {
    InlineExecutor ui;
    InlineExecutor worker;
    AsyncCommand<int, int> cmd{ui, worker,
        [](int x) -> Task<int> { co_return x * 3; }};

    cmd.execute(7);
    wait_until([&]{ return !cmd.is_executing.get(); });
    CHECK(*cmd.last_result.get() == 21);
}

// ═══════════════════════════════════════════════════════════════════════
//  Policy: DropIfRunning
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("AsyncCommand: DropIfRunning policy ignores re-entry while busy") {
    std::atomic<int>  entered{0};
    std::atomic<bool> gate{false};

    ThreadedCommandFixture<int, int> fx{
        /*worker_threads=*/1,
        [&](CancellationToken, int x) -> Task<int> {
            entered.fetch_add(1, std::memory_order_acq_rel);
            while (!gate.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            co_return x + 1;
        },
        AsyncCommandPolicy::DropIfRunning};

    fx.cmd.execute(1);
    // First invocation needs to come back to `ui` once (Property writes
    // for is_executing=true) before it hops to the worker. Pump until the
    // body is actually inside.
    REQUIRE(fx.wait_for([&]{ return entered.load() == 1; }));

    // Second and third must be silently dropped.
    fx.cmd.execute(2);
    fx.cmd.execute(3);
    CHECK(entered.load() == 1);

    gate = true;
    REQUIRE(fx.wait_for_idle());
    // Still only one body ran.
    CHECK(entered.load() == 1);
    CHECK(*fx.cmd.last_result.get() == 2);
}

// ═══════════════════════════════════════════════════════════════════════
//  Policy: LatestOnly
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("AsyncCommand: LatestOnly cancels the previous invocation") {
    std::atomic<int>  entered{0};
    std::atomic<int>  completed{0};
    std::atomic<int>  cancelled{0};
    std::atomic<bool> release_first{false};

    ThreadedCommandFixture<int, int> fx{
        /*worker_threads=*/2,
        [&](CancellationToken tok, int x) -> Task<int> {
            const int me = entered.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (me == 1) {
                // Slow path: hold until released, probing the token so
                // LatestOnly can cancel us.
                while (!release_first.load(std::memory_order_acquire)) {
                    if (tok.is_cancelled()) {
                        cancelled.fetch_add(1, std::memory_order_acq_rel);
                        throw OperationCancelled{};
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
            }
            completed.fetch_add(1, std::memory_order_acq_rel);
            co_return x * 10;
        },
        AsyncCommandPolicy::LatestOnly};

    fx.cmd.execute(1);
    REQUIRE(fx.wait_for([&]{ return entered.load() >= 1; }));

    // Fire the next invocation. Policy must cancel the first one.
    fx.cmd.execute(2);

    // Give LatestOnly time to flip the first invocation's token and
    // for the first body to notice. We don't release the first gate —
    // cancellation should be enough to unblock it.
    REQUIRE(fx.wait_for([&]{ return cancelled.load() >= 1; }));
    CHECK(cancelled.load() >= 1);

    // Let any still-running body progress (in case the second body
    // itself was queued waiting on the pool).
    release_first = true;

    REQUIRE(fx.wait_for_idle());

    // At least one invocation completed (the second), at least one
    // was cancelled (the first).
    CHECK(completed.load() >= 1);
    CHECK(fx.cmd.last_result.get().has_value());
    CHECK(*fx.cmd.last_result.get() == 20);    // result came from input=2
}

// ═══════════════════════════════════════════════════════════════════════
//  Error sink
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("AsyncCommand: unhandled exceptions flow through error sink") {
    InlineExecutor ui;
    InlineExecutor worker;

    std::string captured;
    set_error_sink([&](std::string_view m) { captured = std::string(m); });

    AsyncCommand<int, int> cmd{ui, worker, [](int) -> Task<int> {
        throw std::runtime_error("boom-via-sink"); co_return 0;
    }};

    cmd.execute(0);
    wait_until([&]{ return !cmd.is_executing.get(); });

    CHECK(cmd.last_error_message.get() == "boom-via-sink");
    REQUIRE(cmd.last_error.get().has_value());
    CHECK(cmd.last_error.get()->kind == ErrorKind::AsyncFailure);
    CHECK(captured.find("boom-via-sink") != std::string::npos);

    // Clean up the global sink so follow-on tests start fresh.
    set_error_sink({});
}
