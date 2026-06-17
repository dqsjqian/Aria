// ============================================================================
//  fuzz_cancellation_race.cpp  (L-36)
// ----------------------------------------------------------------------------
//  Invariant under stress:
//    "CancellationSource::cancel() is one-shot and idempotent. Every
//     callback registered via CancellationToken::on_cancel() MUST fire
//     EXACTLY ONCE -- whether it was registered before, during, or
//     after cancel(). Late registrations on an already-cancelled
//     token MUST fire synchronously inside on_cancel()."
//
//  Strategy:
//    - For each iteration: build a fresh source/token pair, register
//      a random number of callbacks on multiple threads while a
//      racing thread fires cancel(). After both threads join,
//      register N more late callbacks. Every callback MUST have run
//      exactly once.
// ============================================================================

#include <doctest/doctest.h>

#include "aria/async/cancellation.hpp"
#include "fuzz_support.hpp"

#include <atomic>
#include <thread>
#include <vector>

using namespace aria;
using namespace aria::async;

TEST_CASE("L-36 fuzz: every on_cancel callback fires exactly once across threads") {
    fuzz::Rng rng{fuzz::seed(0xCA'1CE'1A'7E)};

    // Reduce iters() because each iteration spins up two threads;
    // 5k * 2 threads * (up to 32 callbacks) is plenty.
    const std::size_t iters_capped = std::min(fuzz::iters(), std::size_t{5'000});

    for (std::size_t step = 0; step < iters_capped; ++step) {
        CancellationSource src;
        auto tok = src.token();

        const std::uint32_t pre_cnt   = rng.u32(0, 16);
        const std::uint32_t race_cnt  = rng.u32(0, 16);
        const std::uint32_t post_cnt  = rng.u32(0, 16);
        const std::uint32_t total     = pre_cnt + race_cnt + post_cnt;

        std::vector<std::atomic<int>> hits(total);
        for (auto& h : hits) h.store(0, std::memory_order_relaxed);

        std::uint32_t idx = 0;
        for (std::uint32_t i = 0; i < pre_cnt; ++i, ++idx) {
            tok.on_cancel([&hits, idx]() noexcept {
                hits[idx].fetch_add(1, std::memory_order_relaxed);
            });
        }

        // Racing registrations vs cancel().
        std::atomic<bool> start{false};
        std::thread reg_thread{[&]() {
            while (!start.load(std::memory_order_acquire)) { /* spin */ }
            for (std::uint32_t i = 0; i < race_cnt; ++i) {
                const std::uint32_t k = pre_cnt + i;
                tok.on_cancel([&hits, k]() noexcept {
                    hits[k].fetch_add(1, std::memory_order_relaxed);
                });
            }
        }};
        std::thread cancel_thread{[&]() {
            while (!start.load(std::memory_order_acquire)) { /* spin */ }
            src.cancel();
        }};
        start.store(true, std::memory_order_release);
        reg_thread.join();
        cancel_thread.join();

        idx = pre_cnt + race_cnt;
        for (std::uint32_t i = 0; i < post_cnt; ++i, ++idx) {
            tok.on_cancel([&hits, idx]() noexcept {
                hits[idx].fetch_add(1, std::memory_order_relaxed);
            });
        }

        // Every callback MUST have fired exactly once.
        for (std::uint32_t i = 0; i < total; ++i) {
            REQUIRE_MESSAGE(hits[i].load() == 1,
                            "callback ", i, " fired ", hits[i].load(),
                            " times at step ", step);
        }

        // cancel() is idempotent.
        src.cancel();
        for (std::uint32_t i = 0; i < total; ++i) {
            REQUIRE(hits[i].load() == 1);
        }
    }
}
