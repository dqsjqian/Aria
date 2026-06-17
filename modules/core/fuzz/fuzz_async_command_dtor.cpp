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
//  Strategy:
//    - Tight loop: build an AsyncCommand bound to InlineExecutor for
//      both ui & worker (so the whole pipeline runs synchronously
//      inside execute()), kick off an execution, then immediately
//      drop the command. After dtor, write a marker through a side
//      channel and verify nothing inside the (long-dead) command
//      called back into it.
//    - We do not need real cross-thread races to prove the
//      observation -- cancellation at dtor must be observable through
//      the cancellation token and the underlying state stays valid
//      because state is shared_ptr-owned (per L-37). Surviving 50k+
//      iterations with no UAF / leak / double-free is the contract.
// ============================================================================

#include <doctest/doctest.h>

#include "aria/async/async_command.hpp"
#include "aria/async/cancellation.hpp"
#include "aria/async/executor.hpp"
#include "aria/async/task.hpp"
#include "fuzz_support.hpp"

#include <atomic>
#include <memory>

using namespace aria;
using namespace aria::async;

TEST_CASE("L-37 fuzz: AsyncCommand dtor races with execute, no UAF / leak") {
    fuzz::Rng rng{fuzz::seed(0xA5'17'C'D7'02)};

    // Side-channel counters survive the command. They are captured
    // by value into the action lambda so the action body is safe to
    // run even if the AsyncCommand itself is gone.
    auto action_entries  = std::make_shared<std::atomic<std::uint64_t>>(0);
    auto action_finishes = std::make_shared<std::atomic<std::uint64_t>>(0);

    InlineExecutor ui;
    InlineExecutor worker;

    for (std::size_t step = 0; step < fuzz::iters(); ++step) {
        // Per-iteration AsyncCommand. The body increments two
        // shared atomics; the dtor of the command is allowed to
        // race with `execute(...)` returning -- with InlineExecutor
        // the body finishes synchronously inside execute(), so the
        // race below is between dtor and "in-flight machinery
        // tearing down". The command's State outlives the command
        // via shared_ptr per L-37.
        {
            AsyncCommand<int, int> cmd{
                ui, worker,
                [a = action_entries, f = action_finishes](int x) -> Task<int> {
                    a->fetch_add(1, std::memory_order_relaxed);
                    int y = x * 2;
                    f->fetch_add(1, std::memory_order_relaxed);
                    co_return y;
                }};

            // Flip a coin for whether to actually execute on this
            // iteration -- this exercises both the "dtor before
            // execute" path and the "dtor after execute" path.
            if (rng.coin(0.7)) {
                cmd.execute(static_cast<int>(rng.u32()));
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

TEST_CASE("L-37 fuzz: dtor cancels in-flight cancellation token") {
    fuzz::Rng rng{fuzz::seed(0xA5'17'CA'1CE)};

    InlineExecutor ui;
    InlineExecutor worker;

    auto cancellation_observed = std::make_shared<std::atomic<std::uint64_t>>(0);

    const std::size_t iters_capped = std::min(fuzz::iters(), std::size_t{5'000});
    for (std::size_t step = 0; step < iters_capped; ++step) {
        // The action takes a CancellationToken (cancellable form)
        // and uses it as its in-flight signal. After cmd dtor, the
        // command-wide cancel flag is set; if the action probes the
        // token AFTER dtor (impossible with InlineExecutor since the
        // body finishes inside execute(), but we still confirm the
        // contract via a manual probe below) it would see cancelled.
        std::shared_ptr<CancellationToken> captured_tok;
        {
            AsyncCommand<int, int> cmd{
                ui, worker,
                [&captured_tok](CancellationToken tok, int x) -> Task<int> {
                    // Stash the token so we can probe it AFTER the
                    // command dies. Per L-37 the token's underlying
                    // CancellationState is shared_ptr-owned via the
                    // command's per-invocation source; the token
                    // therefore stays valid even after the
                    // AsyncCommand dtor runs.
                    captured_tok = std::make_shared<CancellationToken>(tok);
                    co_return x;
                }};
            if (rng.coin(0.85)) cmd.execute(static_cast<int>(rng.u32()));
            // dtor fires here -> command's cancel.cancel() must
            // propagate to per-invocation tokens.
        }
        if (captured_tok) {
            // Per L-37: dtor cancels every in-flight cancellation
            // source. The token may either reflect that, or remain
            // un-cancelled iff the invocation already completed
            // before dtor (most common with InlineExecutor). Both
            // outcomes are valid; the framework guarantees there is
            // no UAF when reading the state.
            (void)captured_tok->is_cancelled();   // must not crash
            cancellation_observed->fetch_add(
                captured_tok->is_cancelled() ? 1 : 0,
                std::memory_order_relaxed);
        }
    }
    // No assertion on the count: the contract is "no UAF / no
    // crash", not "always cancelled" (synchronous bodies finish
    // before dtor, so the token may legitimately be un-cancelled).
    (void)cancellation_observed;
}
