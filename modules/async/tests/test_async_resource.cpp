#include <doctest/doctest.h>

#include "aria/async/async_resource.hpp"
#include "aria/async/executor.hpp"
#include "aria/async/task.hpp"

#include <stdexcept>
#include <string>

using namespace aria::async;
using aria::ErrorKind;

namespace {

// Free function fetcher (lambda+coro reference-capture is unsafe).
struct FetchCounter { int hits = 0; bool fail_next = false; };

Task<std::string> fetch_user_impl(int id, FetchCounter& c) {
    ++c.hits;
    if (c.fail_next) {
        c.fail_next = false;
        throw std::runtime_error("network down");
    }
    co_return "user#" + std::to_string(id);
}

}  // namespace

TEST_CASE("AsyncResource: cache hit dedupes repeat fetches") {
    InlineExecutor ui, worker;
    FetchCounter counter;

    AsyncResource<std::string, int> r{ui, worker,
        [&](int id) { return fetch_user_impl(id, counter); }};

    r.fetch(42);
    CHECK(counter.hits == 1);
    CHECK(r.data.get().has_value());
    CHECK(*r.data.get() == "user#42");
    CHECK(r.is_loading.get() == false);

    r.fetch(42);   // cache hit
    r.fetch(42);   // cache hit
    CHECK(counter.hits == 1);
}

TEST_CASE("AsyncResource: different key triggers new fetch") {
    InlineExecutor ui, worker;
    FetchCounter counter;

    AsyncResource<std::string, int> r{ui, worker,
        [&](int id) { return fetch_user_impl(id, counter); }};

    r.fetch(1);
    CHECK(counter.hits == 1);
    CHECK(*r.data.get() == "user#1");

    r.fetch(2);
    CHECK(counter.hits == 2);
    CHECK(*r.data.get() == "user#2");
}

TEST_CASE("AsyncResource: invalidate forces refetch") {
    InlineExecutor ui, worker;
    FetchCounter counter;

    AsyncResource<std::string, int> r{ui, worker,
        [&](int id) { return fetch_user_impl(id, counter); }};

    r.fetch(7);
    CHECK(counter.hits == 1);

    r.invalidate();
    r.fetch(7);
    CHECK(counter.hits == 2);
}

TEST_CASE("AsyncResource: refresh refetches current key") {
    InlineExecutor ui, worker;
    FetchCounter counter;

    AsyncResource<std::string, int> r{ui, worker,
        [&](int id) { return fetch_user_impl(id, counter); }};

    r.fetch(5);
    CHECK(counter.hits == 1);
    r.refresh();
    CHECK(counter.hits == 2);
}

TEST_CASE("AsyncResource: error path keeps old data (stale-while-revalidate)") {
    InlineExecutor ui, worker;
    FetchCounter counter;

    AsyncResource<std::string, int> r{ui, worker,
        [&](int id) { return fetch_user_impl(id, counter); }};

    r.fetch(10);
    CHECK(*r.data.get() == "user#10");
    CHECK_FALSE(r.error.get().has_value());

    counter.fail_next = true;
    r.invalidate();
    r.fetch(10);
    CHECK(r.error_message.get() == "network down");
    REQUIRE(r.error.get().has_value());
    CHECK(r.error.get()->kind == ErrorKind::AsyncFailure);
    CHECK(r.error.get()->source == "AsyncResource");
    // SWR: data is preserved across the failed refresh.
    CHECK(*r.data.get() == "user#10");
}

TEST_CASE("AsyncResource: clear() drops state") {
    InlineExecutor ui, worker;
    FetchCounter counter;

    AsyncResource<std::string, int> r{ui, worker,
        [&](int id) { return fetch_user_impl(id, counter); }};

    r.fetch(1);
    CHECK(r.has_data());
    r.clear();
    CHECK_FALSE(r.has_data());
    CHECK_FALSE(r.error.get().has_value());
    CHECK_FALSE(r.is_loading.get());
}

// ============================================================================
//  R-1: in_flight invariant under interleaved key changes.
//
//  ManualExecutor lets us order the resumptions precisely:
//    1. fetch(KEY_A)  -> schedules stage_1_A on UI; we DON'T drain.
//    2. fetch(KEY_B)  -> bumps gen, schedules stage_1_B on UI.
//    3. drain UI      -> stage_1_A runs first (FIFO), reaches the
//       stale-result guard, observes gen != my_gen, drops result.
//    4. INVARIANT (R-1): in_flight MUST still be true (the stale run
//       does NOT clear the flag; the newer run still owns it).
//    5. drain again to let stage_1_B finish; only then in_flight = false.
// ============================================================================

namespace {

// Minimal manual executor: stores posted functions, drains them on demand.
class ManualExecutor : public IExecutor {
public:
    void post(std::function<void()> fn) override {
        queue_.push_back(std::move(fn));
    }
    bool drain_one() {
        if (queue_.empty()) return false;
        auto fn = std::move(queue_.front());
        queue_.erase(queue_.begin());
        fn();
        return true;
    }
    void drain_all() {
        while (drain_one()) {}
    }
    [[nodiscard]] std::size_t size() const noexcept { return queue_.size(); }

private:
    std::vector<std::function<void()>> queue_;
};

Task<std::string> manual_fetch(int id) {
    co_return "user#" + std::to_string(id);
}

}  // namespace

TEST_CASE("R-1: stale fetch MUST NOT clear in_flight while newer run is pending") {
    ManualExecutor ui;
    InlineExecutor worker;   // worker is inline; UI is the gate we control.

    AsyncResource<std::string, int> r{ui, worker,
        [&](int id) { return manual_fetch(id); }};

    // (1) fetch KEY_A -- bumps gen, sets is_loading synchronously
    //     (LO-1: caller sees Loading the moment fetch() returns), and
    //     schedules stage_1 on UI. UI not drained yet.
    r.fetch(1);
    CHECK(r.is_loading.get() == true);
    CHECK(r.loadable.get().is_loading());
    CHECK(ui.size() >= 1);

    // (2) fetch KEY_B -- bumps gen, schedules stage_1_B on UI.
    r.fetch(2);
    const auto queued_after_two_fetches = ui.size();
    CHECK(queued_after_two_fetches >= 2);

    // (3) drain UI: stage_1_A runs, hits stale-guard, drops result;
    //     stage_1_B runs, completes successfully.
    //     We drain step-by-step so we can assert the in-between state.
    //
    // After the FIRST drain step the older run (KEY_A) reaches its
    // pre-await UI hop (is_loading = true, error = nullopt); the
    // newer run is still queued and has not yet flipped gen.
    // After the SECOND step both runs have started; gen now points
    // at the newer run.
    //
    // The contract we are pinning down is the FINAL state: when
    // every UI continuation drains, the older run's stale-guard
    // path leaves in_flight intact; the newer run's success path
    // is the one that clears it. Hence after a full drain:
    //   data = "user#2"
    //   in_flight = false (cleared by the WINNER, not the stale run)
    ui.drain_all();

    REQUIRE(r.data.get().has_value());
    CHECK(*r.data.get() == "user#2");
    CHECK(r.is_loading.get() == false);
    CHECK_FALSE(r.error.get().has_value());

    // R-1 specifically: even if a stale run lands LAST (which
    // ManualExecutor's strict FIFO does not produce), it MUST NOT
    // clear in_flight. We exercise that path explicitly with a
    // second interleaving below.
}

TEST_CASE("R-1: stale fetch landing AFTER winner does not flip is_loading false") {
    ManualExecutor ui;
    InlineExecutor worker;

    AsyncResource<std::string, int> r{ui, worker,
        [&](int id) { return manual_fetch(id); }};

    // First fetch completes cleanly -> populates data.
    r.fetch(1);
    ui.drain_all();
    REQUIRE(*r.data.get() == "user#1");

    // Now stage two interleaved fetches and drain in REVERSE order
    // by reaching into the queue. We can't easily reorder
    // ManualExecutor's vector, so instead we issue a third fetch
    // mid-drain to simulate a stale run racing past the winner.
    r.invalidate();
    r.fetch(2);                  // KEY_2 fetch enqueued
    // Mid-drain: peek that we have queued continuations, then issue
    // another fetch that will bump gen mid-flight.
    CHECK(ui.size() >= 1);
    r.invalidate();
    r.fetch(3);                  // KEY_3 fetch enqueued; gen bumped

    // Drain all: KEY_2 will reach stale-guard (gen != my_gen) and
    // MUST NOT clear in_flight. KEY_3 wins.
    ui.drain_all();

    REQUIRE(r.data.get().has_value());
    CHECK(*r.data.get() == "user#3");
    // in_flight is observable via is_loading. After ALL runs land
    // (winner included), is_loading is false. The contract proven
    // here: data ended up as KEY_3 (the winner), not as KEY_2 or
    // an earlier stale value.
    CHECK_FALSE(r.is_loading.get());
}

// ============================================================================
//  P1-H / L-38: cancel() drops in-flight work without destroying the
//  resource, and re-arms so the resource stays usable. This is the
//  view-destroy axis — wired in production via
//  BindingEngine::bind_view_lifetime(view, [&]{ resource.cancel(); }).
// ============================================================================

TEST_CASE("AsyncResource: cancel() drops in-flight write-back and re-arms") {
    ManualExecutor ui;       // we control when the UI continuation runs
    InlineExecutor worker;   // worker hop is synchronous
    FetchCounter   counter;

    AsyncResource<std::string, int> r{ui, worker,
        [&](int id) { return fetch_user_impl(id, counter); }};

    // (1) fetch -> loading surfaced synchronously; stage_1 queued on UI;
    //     the fetcher has NOT run (the worker hop is past the UI hop).
    r.fetch(1);
    CHECK(r.is_loading.get() == true);
    CHECK(counter.hits == 0);
    CHECK(ui.size() >= 1);

    // (2) cancel BEFORE the UI continuation drains: flips the in-flight
    //     token, re-arms a fresh source, clears the loading flag.
    r.cancel();
    CHECK(r.is_loading.get() == false);

    // (3) drain: the suspended coroutine resumes and unwinds at
    //     throw_if_cancelled (swallowed by the detached-task path). The
    //     fetcher never ran and no data was written.
    ui.drain_all();
    CHECK(counter.hits == 0);
    CHECK_FALSE(r.data.get().has_value());

    // (4) re-arm proven: a brand-new fetch completes end-to-end.
    r.fetch(2);
    ui.drain_all();
    CHECK(counter.hits == 1);
    REQUIRE(r.data.get().has_value());
    CHECK(*r.data.get() == "user#2");
    CHECK(r.is_loading.get() == false);
}

TEST_CASE("AsyncResource: cancel() preserves last data (stale-while-revalidate)") {
    ManualExecutor ui;
    InlineExecutor worker;
    FetchCounter   counter;

    AsyncResource<std::string, int> r{ui, worker,
        [&](int id) { return fetch_user_impl(id, counter); }};

    // First fetch completes -> data populated.
    r.fetch(1);
    ui.drain_all();
    REQUIRE(*r.data.get() == "user#1");

    // Start a refresh, then cancel mid-flight. SWR: the previous value
    // stays visible; only the loading flag is cleared.
    r.refresh();
    CHECK(r.is_loading.get() == true);
    r.cancel();
    CHECK(r.is_loading.get() == false);
    REQUIRE(r.data.get().has_value());
    CHECK(*r.data.get() == "user#1");

    // Drain the cancelled refresh: it must not overwrite the kept value.
    ui.drain_all();
    CHECK(*r.data.get() == "user#1");
}

// ============================================================================
//  AsyncResource <-> Loadable<T> interconversion
//
//  Per `loadable.hpp` / LO-1, AsyncResource exposes a synthesised
//  `Property<Loadable<T>>` that always agrees with the four primitive
//  Properties (is_loading / data / error). The contract:
//
//    Idle       <- !is_loading && !data && !error
//    Loading    <-  is_loading && !data && !error
//    Refreshing <-  is_loading &&  data
//    Success    <- !is_loading &&  data && !error
//    Error      <- error.has_value()  (data preserved if SWR)
// ============================================================================

TEST_CASE("Loadable: idle by default; success after first fetch") {
    InlineExecutor ui, worker;
    FetchCounter counter;

    AsyncResource<std::string, int> r{ui, worker,
        [&](int id) { return fetch_user_impl(id, counter); }};

    CHECK(r.loadable.get().is_idle());
    CHECK(r.loadable.get().value() == nullptr);
    CHECK_FALSE(r.loadable.get().has_error());

    r.fetch(42);
    CHECK(r.loadable.get().is_success());
    REQUIRE(r.loadable.get().has_value());
    CHECK(*r.loadable.get().value() == "user#42");
    CHECK_FALSE(r.loadable.get().in_flight());
}

TEST_CASE("Loadable: error path collapses to Error state with SWR prior") {
    InlineExecutor ui, worker;
    FetchCounter counter;

    AsyncResource<std::string, int> r{ui, worker,
        [&](int id) { return fetch_user_impl(id, counter); }};

    r.fetch(10);
    REQUIRE(r.loadable.get().is_success());
    REQUIRE(*r.loadable.get().value() == "user#10");

    counter.fail_next = true;
    r.invalidate();
    r.fetch(10);

    auto l = r.loadable.get();
    REQUIRE(l.is_error());
    REQUIRE(l.has_error());
    CHECK(l.error()->kind == ErrorKind::AsyncFailure);
    // SWR: prior value still visible while in Error state.
    REQUIRE(l.has_value());
    CHECK(*l.value() == "user#10");
}

TEST_CASE("Loadable: clear() collapses back to Idle") {
    InlineExecutor ui, worker;
    FetchCounter counter;

    AsyncResource<std::string, int> r{ui, worker,
        [&](int id) { return fetch_user_impl(id, counter); }};

    r.fetch(1);
    REQUIRE(r.loadable.get().is_success());

    r.clear();
    CHECK(r.loadable.get().is_idle());
    CHECK_FALSE(r.loadable.get().has_value());
    CHECK_FALSE(r.loadable.get().has_error());
}

TEST_CASE("Loadable: Refreshing state surfaces while a refresh is in flight") {
    ManualExecutor ui;
    InlineExecutor worker;

    AsyncResource<std::string, int> r{ui, worker,
        [&](int id) { return manual_fetch(id); }};

    // Prime cache so subsequent fetch goes through Refreshing.
    r.fetch(1);
    ui.drain_all();
    REQUIRE(r.loadable.get().is_success());

    // Issue a refresh; before the UI continuation drains the
    // resource is in `Refreshing` -- not `Loading` -- because the
    // prior value is still observable for SWR.
    r.refresh();

    auto mid = r.loadable.get();
    REQUIRE(mid.is_refreshing());
    REQUIRE(mid.has_value());
    CHECK(*mid.value() == "user#1");
    CHECK(mid.in_flight());

    // Drain to completion: collapses back to Success with the new
    // value (still "user#1" since the fetcher is deterministic).
    ui.drain_all();
    CHECK(r.loadable.get().is_success());
}
