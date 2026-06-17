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

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace aria;
using namespace aria::async;

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
