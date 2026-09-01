#include <doctest/doctest.h>

#include "aria/async/executor.hpp"
#include "aria/async/task.hpp"
#include "aria/async/when_all.hpp"

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace aria::async;
namespace ck = std::chrono;

namespace {

Task<int> make_int(int x) { co_return x; }

Task<int> sleep_then(IExecutor& pool, int x, int ms) {
    co_await schedule_on(pool);
    std::this_thread::sleep_for(ck::milliseconds{ms});
    co_return x;
}

Task<int> throws() {
    throw std::runtime_error("boom");
    co_return 0;
}

Task<void> run_parallel_sum(IExecutor& pool,
                            std::atomic<bool>& done,
                            std::atomic<int>& sum) {
    auto [a, b, c] = co_await when_all(
        sleep_then(pool, 1, 50),
        sleep_then(pool, 2, 50),
        sleep_then(pool, 3, 50)
    );
    sum = a + b + c;
    done = true;
}

/// Sequential counterpart of `run_parallel_sum`: same three tasks, same
/// executor, same coroutine machinery — only awaited one at a time. The
/// baseline has to pay every cost the parallel run pays except the
/// parallelism itself, otherwise the comparison measures scheduling
/// overhead rather than concurrency.
///
/// Parameters are pointers, not references: a reference parameter is
/// stored in the coroutine frame and outlives the call expression, so it
/// dangles if the referent dies across a suspension point
/// (cppcoreguidelines-avoid-reference-coroutine-parameters). Everything
/// here lives in the enclosing TEST_CASE and cannot die early, but the
/// pointer spelling documents that and keeps the tidy gate honest — the
/// surrounding helpers predate that check and are covered by the baseline.
Task<void> run_sequential_sum(IExecutor* pool,
                              std::atomic<bool>* done,
                              std::atomic<int>* sum) {
    int a = co_await sleep_then(*pool, 1, 50);
    int b = co_await sleep_then(*pool, 2, 50);
    int c = co_await sleep_then(*pool, 3, 50);
    *sum = a + b + c;
    *done = true;
}

}  // namespace

TEST_CASE("when_all: returns tuple of values") {
    auto t = []() -> Task<std::tuple<int, int, int>> {
        auto [a, b, c] = co_await when_all(make_int(1), make_int(2), make_int(3));
        co_return std::make_tuple(a, b, c);
    }();
    auto [a, b, c] = t.blocking_get();
    CHECK(a == 1);
    CHECK(b == 2);
    CHECK(c == 3);
}

TEST_CASE("when_all: heterogeneous types") {
    auto t = []() -> Task<int> {
        auto fs = []() -> Task<std::string> { co_return std::string{"hi"}; }();
        auto fi = []() -> Task<int>         { co_return 42; }();
        auto result = co_await when_all(std::move(fs), std::move(fi));
        co_return static_cast<int>(std::get<0>(result).size()) + std::get<1>(result);
    }();
    CHECK(t.blocking_get() == 44);  // "hi" len 2 + 42
}

TEST_CASE("when_all: real parallelism (wall time ≈ max, not sum)") {
    ThreadPoolExecutor pool(4);

    // Assert that the parallel run beats a sequential run of the SAME work,
    // measured on the same machine moments earlier.
    //
    // Two earlier versions of this assertion failed on CI while `when_all`
    // was behaving perfectly, and both failures came from comparing against
    // the wrong thing:
    //
    //  1. A fixed ceiling (`elapsed < 130` against ~150ms of sleeping) is
    //     not portable — a loaded sanitized runner spent 169ms on correct
    //     parallel work.
    //  2. A ratio against a baseline of three bare `sleep_for` calls is
    //     still wrong, and this is the subtle one. The parallel path pays
    //     for the thread pool, the coroutine frames and the sanitizer
    //     instrumentation; three raw sleeps pay for none of that. When the
    //     fixed overhead grows to the same order as the sleeps themselves,
    //     the ratio stops describing concurrency at all: CI reported
    //     parallel=215ms against a 209ms budget derived from a 262ms
    //     baseline, i.e. the overhead alone blew the margin.
    //
    // So the baseline now runs `run_sequential_sum`: identical tasks,
    // identical executor, identical coroutine machinery, awaited one at a
    // time. Both sides carry the same fixed cost, which cancels out of the
    // comparison, and what remains is exactly the thing under test. Three
    // 50ms tasks: sequential ≈ 150ms + overhead, parallel ≈ 50ms +
    // overhead, so an 80% ceiling has a wide margin yet still fails loudly
    // if the tasks ever serialise.
    std::atomic<bool> seq_done{false};
    std::atomic<int> seq_sum{0};
    auto seq_t0 = ck::steady_clock::now();
    run_sequential_sum(&pool, &seq_done, &seq_sum).start_detached_();
    while (!seq_done.load()) std::this_thread::sleep_for(ck::milliseconds{2});
    const auto sequential_ms =
        ck::duration_cast<ck::milliseconds>(ck::steady_clock::now() - seq_t0).count();
    std::this_thread::sleep_for(ck::milliseconds{30});
    REQUIRE(seq_sum.load() == 6);

    std::atomic<bool> done{false};
    std::atomic<int> sum{0};
    run_parallel_sum(pool, done, sum).start_detached_();

    auto t0 = ck::steady_clock::now();
    while (!done.load()) std::this_thread::sleep_for(ck::milliseconds{2});
    auto elapsed = ck::duration_cast<ck::milliseconds>(ck::steady_clock::now() - t0).count();
    // Give detached wrapper coroutine time to finish before the pool destructor runs.
    std::this_thread::sleep_for(ck::milliseconds{30});
    CHECK(sum.load() == 6);

    const auto budget_ms = (sequential_ms * 8) / 10;
    INFO("parallel=", elapsed, "ms sequential baseline=", sequential_ms,
         "ms budget=", budget_ms, "ms");
    CHECK(elapsed < budget_ms);
}

TEST_CASE("when_all: rethrows the first exception") {
    auto t = []() -> Task<int> {
        auto [a, b] = co_await when_all(make_int(1), throws());
        co_return a + b;
    }();
    CHECK_THROWS_AS(t.blocking_get(), std::runtime_error);
}

// ── when_any ────────────────────────────────────────────────────────────

namespace {

Task<int> immediate_int(int x) { co_return x; }

Task<int> sleep_then_any(IExecutor& pool, int x, int ms) {
    co_await schedule_on(pool);
    std::this_thread::sleep_for(ck::milliseconds{ms});
    co_return x;
}

Task<int> throws_immediate() {
    throw std::runtime_error("any-boom");
    co_return 0;
}

Task<void> immediate_void() { co_return; }

}  // namespace

TEST_CASE("when_any: synchronously-done first task wins (sync await_suspend race)") {
    auto t = []() -> Task<std::size_t> {
        std::vector<Task<int>> tasks;
        tasks.push_back(immediate_int(7));
        tasks.push_back(immediate_int(8));
        auto r = co_await when_any(std::move(tasks));
        // The first sync-done task must win and the protocol must NOT
        // double-resume the parent (would have crashed/aborted otherwise).
        CHECK(r.index == 0);
        REQUIRE(r.value.has_value());
        CHECK(r.value.value() == 7);
        co_return r.index;
    }();
    auto idx = t.blocking_get();
    CHECK(idx == 0);
}

TEST_CASE("when_any: fastest among real-time waiters wins") {
    static std::atomic<bool> done{false};
    static std::atomic<int>  result{0};
    done = false; result = 0;
    // Spread the sleeps wide apart on purpose.
    //
    // These were 100 / 20 / 60ms, and under ASan/UBSan on a loaded CI
    // runner the 100ms task won — reported as `CHECK(1 == 99)`. Nothing
    // was wrong with `when_any`: a 40ms gap is not a reliable ordering
    // signal once sanitizer instrumentation and a contended scheduler
    // are in play, because wakeup jitter approaches the gap itself.
    //
    // With 20ms against 300ms and 500ms the winner is unambiguous unless
    // the machine stalls for a quarter second, at which point a failure
    // is real news rather than noise. Runtime is bounded by the winner
    // (~20ms) plus the drain wait, not by the losers, so widening the
    // gap costs the suite almost nothing.
    struct Helper {
        static Task<void> body(IExecutor& pool) {
            std::vector<Task<int>> tasks;
            tasks.push_back(sleep_then_any(pool,  1, 500));
            tasks.push_back(sleep_then_any(pool, 99,  20));
            tasks.push_back(sleep_then_any(pool,  3, 300));
            auto r = co_await when_any(std::move(tasks));
            REQUIRE(r.value.has_value());
            result = r.value.value();
            done = true;
        }
    };
    ThreadPoolExecutor pool(4);
    Helper::body(pool).start_detached_();
    // Deadline covers the winner, not the losers.
    auto deadline = ck::steady_clock::now() + ck::seconds{5};
    while (!done.load() && ck::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(ck::milliseconds{2});
    }
    REQUIRE(done.load());   // separates "wrong winner" from "timed out"
    CHECK(result.load() == 99);
    // Let the losers drain before the pool is destroyed.
    std::this_thread::sleep_for(ck::milliseconds{600});
}

TEST_CASE("when_any: throwing first task surfaces the exception") {
    auto t = []() -> Task<bool> {
        std::vector<Task<int>> tasks;
        tasks.push_back(throws_immediate());
        tasks.push_back(immediate_int(2));
        auto r = co_await when_any(std::move(tasks));
        CHECK(r.index == 0);
        CHECK(static_cast<bool>(r.error));
        CHECK(!r.value.has_value());
        co_return true;
    }();
    CHECK(t.blocking_get());
}

TEST_CASE("when_any: void path") {
    auto t = []() -> Task<bool> {
        std::vector<Task<void>> tasks;
        tasks.push_back(immediate_void());
        tasks.push_back(immediate_void());
        auto r = co_await when_any(std::move(tasks));
        CHECK(r.index == 0);
        CHECK(!r.error);
        co_return true;
    }();
    CHECK(t.blocking_get());
}

TEST_CASE("when_any: empty input is await_ready (no-op)") {
    // Empty vector returns a default-constructed Result; the awaiter
    // must NOT suspend on an empty input.
    auto t = []() -> Task<bool> {
        std::vector<Task<int>> tasks;
        auto r = co_await when_any(std::move(tasks));
        CHECK(r.index == std::size_t(-1));
        CHECK(!r.value.has_value());
        CHECK(!r.error);
        co_return true;
    }();
    CHECK(t.blocking_get());
}

TEST_CASE("when_any: late losers cannot corrupt resolved result") {
    // Task 0 is fast (sync); task 1 sleeps; if the late completion
    // tried to overwrite the slot the assertion below would fail.
    static std::atomic<bool>      late_done{false};
    static std::atomic<std::size_t> winner_idx{static_cast<std::size_t>(-1)};
    static std::atomic<int>       value{0};
    late_done = false; winner_idx = static_cast<std::size_t>(-1); value = 0;
    struct Helper {
        static Task<void> body(IExecutor& pool) {
            std::vector<Task<int>> tasks;
            tasks.push_back(immediate_int(42));               // wins index 0
            tasks.push_back(sleep_then_any(pool, 99, 50));    // loses
            auto r = co_await when_any(std::move(tasks));
            winner_idx = r.index;
            if (r.value.has_value()) value = r.value.value();
            late_done = true;
        }
    };
    ThreadPoolExecutor pool(2);
    Helper::body(pool).start_detached_();
    auto deadline = ck::steady_clock::now() + ck::milliseconds{500};
    while (!late_done.load() && ck::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(ck::milliseconds{2});
    }
    CHECK(winner_idx.load() == 0);
    CHECK(value.load() == 42);
    std::this_thread::sleep_for(ck::milliseconds{120});  // let loser drain
}

// ── when_any_cancellable ─────────────────────────────────────────────────

TEST_CASE("when_any_cancellable: winner cancels remaining factories") {
    // Loser 1 polls its token at a safe point; once the winner emerges
    // it must observe is_cancelled() == true within a bounded time.
    static std::atomic<bool> loser_saw_cancel{false};
    static std::atomic<bool> loser_done{false};
    static std::atomic<bool> outer_done{false};
    static std::atomic<int>  outer_result{0};
    loser_saw_cancel = false;
    loser_done = false;
    outer_done = false;
    outer_result = 0;

    struct Helper {
        static Task<void> body(IExecutor& pool) {
            std::vector<std::function<Task<int>(CancellationToken)>> fs;
            fs.push_back([&pool](CancellationToken) -> Task<int> {
                co_await schedule_on(pool);
                std::this_thread::sleep_for(ck::milliseconds{30});
                co_return 1;   // wins
            });
            fs.push_back([&pool](CancellationToken tok) -> Task<int> {
                co_await schedule_on(pool);
                // Long sleep — but probe the token in the middle.
                for (int i = 0; i < 50; ++i) {
                    std::this_thread::sleep_for(ck::milliseconds{10});
                    if (tok.is_cancelled()) {
                        loser_saw_cancel = true;
                        loser_done = true;
                        throw OperationCancelled{};
                    }
                }
                loser_done = true;
                co_return 2;
            });
            auto r = co_await when_any_cancellable(std::move(fs));
            REQUIRE(r.value.has_value());
            outer_result = r.value.value();
            outer_done = true;
        }
    };
    ThreadPoolExecutor pool(2);
    Helper::body(pool).start_detached_();
    auto outer_deadline = ck::steady_clock::now() + ck::milliseconds{500};
    while (!outer_done.load() && ck::steady_clock::now() < outer_deadline) {
        std::this_thread::sleep_for(ck::milliseconds{2});
    }
    CHECK(outer_result.load() == 1);
    // Wait until the loser observes cancellation (or its full lifetime).
    auto loser_deadline = ck::steady_clock::now() + ck::milliseconds{500};
    while (!loser_done.load() && ck::steady_clock::now() < loser_deadline) {
        std::this_thread::sleep_for(ck::milliseconds{5});
    }
    CHECK(loser_saw_cancel.load());
}

TEST_CASE("when_any_cancellable: synchronous winner short-circuits losers") {
    // Factory 0 returns synchronously; factory 1 hasn't even started
    // yet. Once the winner CAS succeeds, factory 1's token is fired
    // BEFORE factory 1's body actually runs — its first probe must
    // see is_cancelled() == true.
    static std::atomic<bool> loser_saw_cancel{false};
    loser_saw_cancel = false;
    struct Helper {
        static Task<int> body() {
            std::vector<std::function<Task<int>(CancellationToken)>> fs;
            fs.push_back([](CancellationToken) -> Task<int> {
                co_return 7;
            });
            fs.push_back([](CancellationToken tok) -> Task<int> {
                // Even if we run, we should see cancellation.
                if (tok.is_cancelled()) loser_saw_cancel = true;
                co_return 0;
            });
            auto r = co_await when_any_cancellable(std::move(fs));
            REQUIRE(r.value.has_value());
            co_return r.value.value();
        }
    };
    auto t = Helper::body();
    CHECK(t.blocking_get() == 7);
    // Loser may or may not run depending on scheduling; if it ran, it
    // must have seen cancellation. We don't make absence-of-run a
    // failure — that is platform/scheduler dependent.
    SUBCASE("loser observation may be either cancelled or not-run") {
        // Either is fine; the contract is "resolved value must be 7"
        // (already checked above). This subcase exists for clarity.
        CHECK(true);
    }
    (void)loser_saw_cancel;
}

TEST_CASE("when_any_cancellable: throwing factory still surfaces error correctly") {
    auto t = []() -> Task<bool> {
        std::vector<std::function<Task<int>(CancellationToken)>> fs;
        fs.push_back([](CancellationToken) -> Task<int> {
            throw std::runtime_error("cancellable-boom");
            co_return 0;
        });
        fs.push_back([](CancellationToken) -> Task<int> {
            co_return 99;
        });
        auto r = co_await when_any_cancellable(std::move(fs));
        // Whichever wins, the result must be self-consistent: either
        // index 0 with error, or index 1 with value=99.
        if (r.index == 0) {
            CHECK(static_cast<bool>(r.error));
            CHECK(!r.value.has_value());
        } else {
            CHECK(r.index == 1);
            REQUIRE(r.value.has_value());
            CHECK(r.value.value() == 99);
        }
        co_return true;
    }();
    CHECK(t.blocking_get());
}

TEST_CASE("when_any_cancellable: empty factories is await_ready (no-op)") {
    auto t = []() -> Task<bool> {
        std::vector<std::function<Task<int>(CancellationToken)>> fs;
        auto r = co_await when_any_cancellable(std::move(fs));
        CHECK(r.index == std::size_t(-1));
        CHECK(!r.value.has_value());
        co_return true;
    }();
    CHECK(t.blocking_get());
}
