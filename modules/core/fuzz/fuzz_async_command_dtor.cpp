// ============================================================================
//  fuzz_async_command_dtor.cpp  (L-37)
// ----------------------------------------------------------------------------
//  Invariant under stress:
//    "Destroying an AsyncCommand at any moment relative to in-flight
//     execute() calls MUST NOT crash, leak, or call back into freed
//     memory. The Property writes inside in-flight coroutines remain
//     safe because state is shared_ptr-owned and outlives the
//     command. After dtor, no new invocations may start."
//
//  Preserve the synchronous lifetime baseline, then destroy commands with
//  actions still queued or genuinely suspended. Manual UI pumping and
//  virtual deadlines make each interleaving deterministic. Retained tokens,
//  completion counters and shared ownership expose missed cancellation and
//  leaked frames without relying on sleeps or absence of sanitizer reports.
// ============================================================================

#include <doctest/doctest.h>

#include "aria/async/async_command.hpp"
#include "aria/async/cancellation.hpp"
#include "aria/async/executor.hpp"
#include "aria/async/task.hpp"
#include "aria/async/virtual_time_executor.hpp"
#include "fuzz_support.hpp"

#include <atomic>
#include <memory>
#include <vector>

using namespace aria;
using namespace aria::async;

TEST_CASE("L-37 fuzz: AsyncCommand synchronous execute and destruction accounting") {
    fuzz::Rng rng{fuzz::seed(0xA5'17'C'D7'02)};

    // Side-channel counters survive the command. They are captured
    // by value into the action lambda so the action body is safe to
    // run even if the AsyncCommand itself is gone.
    auto action_entries  = std::make_shared<std::atomic<std::uint64_t>>(0);
    auto action_finishes = std::make_shared<std::atomic<std::uint64_t>>(0);

    InlineExecutor ui;
    InlineExecutor worker;

    for (std::size_t step = 0; step < fuzz::iters(); ++step) {
        // This is the already-completed baseline; the later cases exercise
        // destruction with work still in flight.
        {
            AsyncCommand<int, int> cmd{
                ui, worker,
                [a = action_entries, f = action_finishes](int x) -> Task<int> {
                    a->fetch_add(1, std::memory_order_relaxed);
                    int y = x * 2;
                    f->fetch_add(1, std::memory_order_relaxed);
                    co_return y;
                }};

            // Execute once deterministically, then alternate randomly between
            // destruction before and after execution. A one-iteration
            // run still exercises the nonempty accounting path.
            if (step == 0 || rng.coin(0.7)) {
                // Bounded so the action body's `x * 2` cannot overflow.
                // A full-range u32 cast to int yields large negatives,
                // and doubling those is signed overflow — real UB that
                // UBSan aborts on, which took the whole fuzz binary
                // down under the asan flavor. The argument value is
                // incidental to the command lifetime being checked.
                cmd.execute(static_cast<int>(rng.u32(0, 1'000'000)));
            }
            // dtor at end of scope: must not crash regardless.
        }
        // After dtor, the action's atomics still live because they
        // were captured by shared_ptr value into the action.
    }

    // Sanity: at least some executions actually ran (otherwise the
    // fuzzer is not exercising anything). The action either runs
    // fully (entry == finish) or not at all -- never half.
    CHECK(action_entries->load() == action_finishes->load());
    CHECK(action_entries->load() > 0);
}

namespace {
struct CommandCounts {
    std::size_t entries = 0;
    std::size_t exits = 0;
    std::size_t cancelled = 0;
    std::vector<CancellationToken> tokens;
};

struct CommandGate {
    std::coroutine_handle<> parked;
    void resume() {
        if (auto handle = std::exchange(parked, {})) handle.resume();
    }
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> handle) noexcept { parked = handle; }
    void await_resume() const noexcept {}
};

template<typename R>
Task<R> parked_command_action(CancellationToken token,
                              VirtualTimeExecutor& timer,
                              std::shared_ptr<CommandCounts> counts,
                              bool cooperative = false) {
    ++counts->entries;
    counts->tokens.push_back(token);
    auto gate = std::make_shared<CommandGate>();
    // The deadline is also failure-path cleanup if destructor cancellation
    // regresses. It becomes a no-op after a cooperative cancellation resume.
    timer.post_after(std::chrono::milliseconds{1}, [gate] { gate->resume(); });
    if (cooperative) {
        token.on_cancel([weak = std::weak_ptr<CommandGate>{gate}] {
            if (auto pending = weak.lock()) pending->resume();
        });
    }
    co_await *gate;
    ++counts->exits;
    if (token.is_cancelled()) ++counts->cancelled;
    token.throw_if_cancelled();
    if constexpr (std::is_void_v<R>) co_return;
    else co_return 7;
}

template<typename R>
void check_parked_destruction(fuzz::Rng& rng, bool cooperative) {
    MainThreadExecutor ui;
    InlineExecutor worker;
    VirtualTimeExecutor timer;
    auto counts = std::make_shared<CommandCounts>();
    auto command = std::make_unique<AsyncCommand<R>>(ui, worker,
        [&timer, counts, cooperative](CancellationToken token) {
            return parked_command_action<R>(std::move(token), timer, counts, cooperative);
        });
    bool settled = false;
    auto subscription = command->is_executing.on_changed([&](bool executing) {
        if (!executing) settled = true;
    });
    const auto n = rng.u32(1, 4);
    for (std::uint32_t i = 0; i < n; ++i) command->execute();
    ui.drain();
    REQUIRE(counts->entries == n);
    REQUIRE(counts->exits == 0);
    REQUIRE(counts->tokens.size() == n);
    REQUIRE(command->is_executing.get());
    for (auto& token : counts->tokens) {
        CHECK_FALSE(token.is_cancelled());
        // Registered after the action's resume callback. Pumping here makes
        // Invocation destruction re-enter m_sources during source.cancel().
        if (cooperative) token.on_cancel([&ui] { ui.drain(); });
    }

    command.reset();
    // Check before advancing time: a token becoming cancelled only when the
    // invocation later destroys its source does not satisfy dtor cancellation.
    for (const auto& token : counts->tokens) CHECK(token.is_cancelled());
    CHECK(counts->exits == (cooperative ? n : 0));
    timer.run_until_idle();
    ui.drain();
    CHECK(counts->exits == n);
    CHECK(counts->cancelled == n);
    CHECK(settled);
    CHECK(timer.pending() == 0);
    CHECK(ui.pending() == 0);
    CHECK(counts.use_count() == 1);
}
}  // namespace

TEST_CASE("L-37 fuzz: dtor cancels every genuinely suspended invocation token") {
    fuzz::Rng rng{fuzz::seed(0xA5'17'CA'1CE)};
    const auto capped = std::min(fuzz::iters(), std::size_t{5'000});
    for (std::size_t step = 0; step < capped; ++step) {
        for (bool cooperative : {false, true}) {
            check_parked_destruction<int>(rng, cooperative);
            check_parked_destruction<void>(rng, cooperative);
        }
    }
}

TEST_CASE("L-37 fuzz: destruction before queued UI start prevents action entry") {
    fuzz::Rng rng{fuzz::seed(0xA5'17'D7'03)};
    const auto capped = std::min(fuzz::iters(), std::size_t{5'000});
    for (std::size_t step = 0; step < capped; ++step) {
        MainThreadExecutor ui;
        InlineExecutor worker;
        VirtualTimeExecutor timer;
        auto counts = std::make_shared<CommandCounts>();
        auto command = std::make_unique<AsyncCommand<int>>(ui, worker,
            [&timer, counts](CancellationToken token) {
                return parked_command_action<int>(std::move(token), timer, counts);
            });
        const auto n = rng.u32(1, 4);
        for (std::uint32_t i = 0; i < n; ++i) command->execute();
        CHECK(counts->entries == 0);
        CHECK(ui.pending() == n);
        command.reset();
        ui.drain();
        CHECK(counts->entries == 0);
        CHECK(counts->exits == 0);
        CHECK(timer.pending() == 0);
        CHECK(ui.pending() == 0);
        CHECK(counts.use_count() == 1);
    }
}
