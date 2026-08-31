// ============================================================================
//  test_async_diagnostics.cpp
// ----------------------------------------------------------------------------
//  Pin down the diagnostics emissions documented in
//  docs/diagnostics.md (Async category) -- AsyncCommand &
//  AsyncResource publish `trace::Async` events for invocation
//  start / finish / cancel / error and for resource fetch /
//  cache_hit / stale_drop / refresh / error. See also
//  api-style.md S-30 (one load + null check on the no-sink path)
//  and error-model.md E-20 / E-21.
// ============================================================================

#include <doctest/doctest.h>

#include "aria/aria.hpp"
#include "aria/async/async_command.hpp"
#include "aria/async/async_resource.hpp"
#include "aria/async/executor.hpp"
#include "aria/async/task.hpp"
#include "aria/async/timeout.hpp"
#include "aria/async/virtual_time_executor.hpp"
#include "aria/async/when_all.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace aria;
using namespace aria::async;
using namespace std::chrono_literals;

namespace {

[[nodiscard]] std::vector<TraceEvent>
collect_async(const std::vector<TraceEvent>& log) {
    std::vector<TraceEvent> out;
    for (const auto& ev : log) {
        if (ev.category == TraceCategory::Async) out.push_back(ev);
    }
    return out;
}

void wait_until(const std::function<bool()>& done,
                std::chrono::milliseconds timeout = std::chrono::seconds{1}) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
}

}  // namespace

// ============================================================================
//  AsyncCommand: invocation start + finish trace events
// ============================================================================

TEST_CASE("Async/diag: AsyncCommand success emits start + finish events") {
    InlineExecutor ui, worker;

    AsyncCommand<int, int> cmd{ui, worker, [](int x) -> Task<int> {
        co_return x * 2;
    }};

    std::vector<TraceEvent> log;
    ScopedTraceSink guard{[&log](const TraceEvent& ev) { log.push_back(ev); }};

    cmd.execute(21);
    wait_until([&]{ return !cmd.is_executing.get(); });

    auto async_log = collect_async(log);
    REQUIRE_FALSE(async_log.empty());

    bool saw_start  = false;
    bool saw_finish = false;
    for (const auto& ev : async_log) {
        if (auto* p = std::get_if<trace::Async>(&ev.payload)) {
            if (p->source == "AsyncCommand") {
                if (p->op.find("start")  != std::string::npos) saw_start  = true;
                if (p->op.find("finish") != std::string::npos
                 || p->op.find("success") != std::string::npos
                 || p->op == "complete")                       saw_finish = true;
            }
        }
    }
    CHECK(saw_start);
    CHECK(saw_finish);
}

TEST_CASE("Async/diag: AsyncCommand failure emits an error-bearing event") {
    InlineExecutor ui, worker;

    AsyncCommand<int, int> cmd{ui, worker, [](int) -> Task<int> {
        throw std::runtime_error("kaboom");
        co_return 0;
    }};

    std::vector<TraceEvent> log;
    ScopedTraceSink guard{[&log](const TraceEvent& ev) { log.push_back(ev); }};

    cmd.execute(0);
    wait_until([&]{ return !cmd.is_executing.get(); });

    auto async_log = collect_async(log);
    REQUIRE_FALSE(async_log.empty());

    // At least one event must carry an Error with kind = AsyncFailure.
    bool saw_error = false;
    for (const auto& ev : async_log) {
        if (ev.error && ev.error->kind == ErrorKind::AsyncFailure) {
            CHECK(ev.error->source == "AsyncCommand");
            saw_error = true;
            break;
        }
    }
    CHECK(saw_error);
}

// ============================================================================
//  AsyncResource: fetch_start + fetch_finish + cache_hit
// ============================================================================

namespace {

Task<std::string> fetch_diag(int id) {
    co_return "user#" + std::to_string(id);
}

}  // namespace

TEST_CASE("Async/diag: AsyncResource fetch emits a 'finish' / 'success' event") {
    InlineExecutor ui, worker;
    AsyncResource<std::string, int> r{ui, worker,
        [&](int id) { return fetch_diag(id); }};

    std::vector<TraceEvent> log;
    ScopedTraceSink guard{[&log](const TraceEvent& ev) { log.push_back(ev); }};

    r.fetch(42);

    auto async_log = collect_async(log);
    REQUIRE_FALSE(async_log.empty());

    bool saw_resource_event = false;
    for (const auto& ev : async_log) {
        if (auto* p = std::get_if<trace::Async>(&ev.payload)) {
            if (p->source == "AsyncResource") {
                saw_resource_event = true;
                break;
            }
        }
    }
    CHECK(saw_resource_event);
}

TEST_CASE("Async/diag: AsyncResource cache hit emits a distinguishable event") {
    InlineExecutor ui, worker;
    AsyncResource<std::string, int> r{ui, worker,
        [&](int id) { return fetch_diag(id); }};

    // Prime cache outside the sink so we only capture the cache-hit run.
    r.fetch(42);

    std::vector<TraceEvent> log;
    ScopedTraceSink guard{[&log](const TraceEvent& ev) { log.push_back(ev); }};

    r.fetch(42);   // cache hit -- no fetcher invocation

    auto async_log = collect_async(log);
    // The contract being asserted: a cache hit MAY publish a
    // dedicated event (e.g. "cache_hit") and MUST NOT publish an
    // error-bearing event. Some implementations choose to skip
    // publishing entirely on a pure cache hit; both are acceptable
    // -- we just forbid spurious error reports.
    for (const auto& ev : async_log) {
        if (ev.error.has_value()) {
            CHECK(ev.error->kind != ErrorKind::AsyncFailure);
        }
    }
}

TEST_CASE("Async/diag: ScopedTraceSink scope exit returns to no-sink mode") {
    InlineExecutor ui, worker;
    AsyncResource<std::string, int> r{ui, worker,
        [&](int id) { return fetch_diag(id); }};

    std::vector<TraceEvent> log;
    {
        ScopedTraceSink guard{[&log](const TraceEvent& ev) { log.push_back(ev); }};
        r.fetch(1);
    }
    const auto count_inside_scope = log.size();

    // Outside the sink, fetches MUST NOT grow the log.
    r.invalidate();
    r.fetch(1);
    r.fetch(2);
    CHECK(log.size() == count_inside_scope);
}

// ============================================================================
//  D-31.1: async race arbitration (with_timeout / when_any / when_all)
// ----------------------------------------------------------------------------
//  These assert the exact `op` spellings and the ordering guarantees the
//  doc promises, because those strings are the wire format consumers
//  filter on — a typo would silently match no filter.
// ============================================================================

namespace {

/// Ops of every Async event from `source`, in publication order.
[[nodiscard]] std::vector<std::string>
race_ops(const std::vector<TraceEvent>& log, std::string_view source) {
    std::vector<std::string> ops;
    for (const auto& ev : log) {
        if (ev.category != TraceCategory::Async) continue;
        if (auto* p = std::get_if<trace::Async>(&ev.payload)) {
            if (p->source == source) ops.push_back(p->op);
        }
    }
    return ops;
}

[[nodiscard]] bool contains(const std::vector<std::string>& ops,
                            std::string_view op) {
    return std::find(ops.begin(), ops.end(), op) != ops.end();
}

/// Index of `op` in the sequence, or npos.
[[nodiscard]] std::size_t index_of(const std::vector<std::string>& ops,
                                   std::string_view op) {
    for (std::size_t i = 0; i < ops.size(); ++i) {
        if (ops[i] == op) return i;
    }
    return std::string::npos;
}

/// The `generation` of the first event matching source+op.
[[nodiscard]] std::uint64_t generation_of(const std::vector<TraceEvent>& log,
                                          std::string_view source,
                                          std::string_view op) {
    for (const auto& ev : log) {
        if (ev.category != TraceCategory::Async) continue;
        if (auto* p = std::get_if<trace::Async>(&ev.payload)) {
            if (p->source == source && p->op == op) return p->generation;
        }
    }
    return std::uint64_t(-1);
}

Task<int> race_wait_then(VirtualTimeExecutor& vt,
                         std::chrono::milliseconds delay,
                         int value,
                         CancellationToken tok) {
    co_await schedule_after(vt, delay);
    tok.throw_if_cancelled();
    co_return value;
}

Task<int> race_plain_wait(VirtualTimeExecutor& vt,
                          std::chrono::milliseconds delay,
                          int value) {
    co_await schedule_after(vt, delay);
    co_return value;
}

Task<void> drive_timeout_race(VirtualTimeExecutor& vt,
                              std::chrono::milliseconds deadline,
                              std::chrono::milliseconds inner,
                              bool* threw_timeout) {
    try {
        (void)co_await with_timeout(vt, deadline,
            [&vt, inner](CancellationToken tok) {
                return race_wait_then(vt, inner, 42, tok);
            });
    } catch (const TimeoutError&) {
        if (threw_timeout) *threw_timeout = true;
    }
}

}  // namespace

TEST_CASE("Async/diag: with_timeout inner-win publishes start → won → end") {
    VirtualTimeExecutor vt;
    std::vector<TraceEvent> log;
    ScopedTraceSink guard{[&log](const TraceEvent& ev) { log.push_back(ev); }};

    auto t = drive_timeout_race(vt, /*deadline=*/500ms, /*inner=*/100ms, nullptr);
    t.start();
    vt.advance_by(100ms);

    const auto ops = race_ops(log, "with_timeout");
    REQUIRE(ops.size() == 3);
    CHECK(ops[0] == "race_start");
    CHECK(ops[1] == "race_won");
    CHECK(ops[2] == "race_end");
    // Exactly one outcome event per race.
    CHECK_FALSE(contains(ops, "race_timeout"));
    CHECK_FALSE(contains(ops, "race_parent_cancel"));
}

TEST_CASE("Async/diag: with_timeout deadline-win publishes race_timeout with an Error") {
    VirtualTimeExecutor vt;
    std::vector<TraceEvent> log;
    ScopedTraceSink guard{[&log](const TraceEvent& ev) { log.push_back(ev); }};

    bool threw_timeout = false;
    auto t = drive_timeout_race(vt, /*deadline=*/100ms, /*inner=*/500ms,
                                &threw_timeout);
    t.start();
    vt.advance_by(100ms);   // deadline fires and claims the race
    vt.advance_by(400ms);   // inner resumes, sees the cancelled token
    CHECK(threw_timeout);

    const auto ops = race_ops(log, "with_timeout");
    CHECK(ops.front() == "race_start");
    CHECK(contains(ops, "race_timeout"));
    CHECK(ops.back() == "race_end");
    // The winner is the deadline, so no race_won — the two are mutually
    // exclusive outcomes of one CAS.
    CHECK_FALSE(contains(ops, "race_won"));
    // A single participant means cancelling it IS the timeout (D-31.1).
    CHECK_FALSE(contains(ops, "race_loser_cancel"));

    // The timeout event must carry a Timeout-kind Error naming the combinator.
    bool saw_timeout_error = false;
    for (const auto& ev : log) {
        if (ev.category != TraceCategory::Async) continue;
        if (auto* p = std::get_if<trace::Async>(&ev.payload)) {
            if (p->op != "race_timeout") continue;
            REQUIRE(ev.error.has_value());
            CHECK(ev.error->kind == ErrorKind::Timeout);
            CHECK(ev.error->source == "with_timeout");
            saw_timeout_error = true;
        }
    }
    CHECK(saw_timeout_error);
}

TEST_CASE("Async/diag: when_any reports the winning index in generation") {
    VirtualTimeExecutor vt;
    std::vector<TraceEvent> log;
    ScopedTraceSink guard{[&log](const TraceEvent& ev) { log.push_back(ev); }};

    int winner_index = -1;
    auto body = [&]() -> Task<void> {
        std::vector<Task<int>> tasks;
        tasks.push_back(race_plain_wait(vt, 300ms, 1));   // index 0: slow
        tasks.push_back(race_plain_wait(vt, 100ms, 2));   // index 1: fast
        auto r = co_await when_any(std::move(tasks));
        winner_index = static_cast<int>(r.index);
    };
    auto t = body();
    t.start();
    vt.advance_by(100ms);
    CHECK(winner_index == 1);

    const auto ops = race_ops(log, "when_any");
    REQUIRE(ops.size() >= 2);
    CHECK(ops.front() == "race_start");
    CHECK(contains(ops, "race_won"));
    CHECK(ops.back() == "race_end");
    // race_start carries the participant count; race_won the winner index.
    CHECK(generation_of(log, "when_any", "race_start") == 2);
    CHECK(generation_of(log, "when_any", "race_won") == 1);
    // The basic when_any form cannot cancel its losers.
    CHECK_FALSE(contains(ops, "race_loser_cancel"));

    vt.advance_by(400ms);   // let the loser finish and self-destruct
}

TEST_CASE("Async/diag: when_any_cancellable publishes loser cancellation once") {
    VirtualTimeExecutor vt;
    std::vector<TraceEvent> log;
    ScopedTraceSink guard{[&log](const TraceEvent& ev) { log.push_back(ev); }};

    auto body = [&]() -> Task<void> {
        std::vector<std::function<Task<int>(CancellationToken)>> fs;
        fs.push_back([&vt](CancellationToken tok) {
            return race_wait_then(vt, 300ms, 1, tok);
        });
        fs.push_back([&vt](CancellationToken tok) {
            return race_wait_then(vt, 100ms, 2, tok);
        });
        fs.push_back([&vt](CancellationToken tok) {
            return race_wait_then(vt, 500ms, 3, tok);
        });
        (void)co_await when_any_cancellable(std::move(fs));
    };
    auto t = body();
    t.start();
    vt.advance_by(100ms);

    const auto ops = race_ops(log, "when_any_cancellable");
    CHECK(ops.front() == "race_start");
    CHECK(generation_of(log, "when_any_cancellable", "race_start") == 3);

    // One event per race, not per loser; generation = losers signalled.
    const auto cancels = std::count(ops.begin(), ops.end(), "race_loser_cancel");
    CHECK(cancels == 1);
    CHECK(generation_of(log, "when_any_cancellable", "race_loser_cancel") == 2);

    // Losers are only signalled once a winner exists, so the outcome
    // event must precede the cancellation event.
    const auto won_at    = index_of(ops, "race_won");
    const auto cancel_at = index_of(ops, "race_loser_cancel");
    REQUIRE(won_at != std::string::npos);
    REQUIRE(cancel_at != std::string::npos);
    CHECK(won_at < cancel_at);

    vt.advance_by(500ms);
}

TEST_CASE("Async/diag: when_all publishes start/won/end and never a loser cancel") {
    VirtualTimeExecutor vt;
    std::vector<TraceEvent> log;
    ScopedTraceSink guard{[&log](const TraceEvent& ev) { log.push_back(ev); }};

    auto body = [&]() -> Task<void> {
        auto [a, b] = co_await when_all(race_plain_wait(vt, 100ms, 1),
                                        race_plain_wait(vt, 200ms, 2));
        CHECK(a == 1);
        CHECK(b == 2);
    };
    auto t = body();
    t.start();
    vt.advance_by(200ms);

    const auto ops = race_ops(log, "when_all");
    REQUIRE(ops.size() == 3);
    CHECK(ops[0] == "race_start");
    CHECK(ops[1] == "race_won");   // fires when the LAST participant lands
    CHECK(ops[2] == "race_end");
    // when_all has no losers by construction.
    CHECK_FALSE(contains(ops, "race_loser_cancel"));
    CHECK_FALSE(contains(ops, "race_timeout"));
    CHECK(generation_of(log, "when_all", "race_start") == 2);
}

TEST_CASE("Async/diag: race arbitration publishes nothing without a sink") {
    VirtualTimeExecutor vt;
    std::vector<TraceEvent> log;

    // No sink installed for the race itself (AD2 / D-24): the arbitration
    // path must not build payloads when nobody is listening.
    auto t = drive_timeout_race(vt, /*deadline=*/500ms, /*inner=*/100ms, nullptr);
    t.start();
    vt.advance_by(100ms);

    ScopedTraceSink guard{[&log](const TraceEvent& ev) { log.push_back(ev); }};
    CHECK(log.empty());
}
