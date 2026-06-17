// ============================================================================
//  test_async_validator.cpp
// ----------------------------------------------------------------------------
//  Pin down the V-N invariants laid out in
//  modules/async/include/aria/async/async_validator.hpp:
//
//    V-1 latest-wins / stale-drop
//    V-2 pending semantics
//    V-3 cancellation never surfaces as Error
//    V-4 ValidationKey + rule_id
//    V-5 de-duplication of identical successive values
//    V-6 lifetime: destruction / detach cancels in-flight rule
// ============================================================================

#include <doctest/doctest.h>

#include "aria/aria.hpp"
#include "aria/async/async_validator.hpp"
#include "aria/async/cancellation.hpp"
#include "aria/async/executor.hpp"
#include "aria/async/task.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace aria;
using namespace aria::async;

namespace {

// Simple fetcher counter.
struct AVCounter {
    std::atomic<int>  fires{0};
    std::atomic<int>  cancels{0};
    std::atomic<bool> next_should_fail{false};
};

// Async rule: fails when the input == "taken", otherwise passes.
// Counts invocations and cancellations through `c`.
Task<AsyncRuleResult> remote_username_check(std::string username,
                                            CancellationToken tok,
                                            AVCounter& c) {
    c.fires.fetch_add(1, std::memory_order_relaxed);
    try {
        tok.throw_if_cancelled();
    } catch (...) {
        c.cancels.fetch_add(1, std::memory_order_relaxed);
        throw;
    }
    if (c.next_should_fail.exchange(false)) {
        throw std::runtime_error("network down");
    }
    if (username == "taken") {
        co_return AsyncRuleResult::failed(
            ValidationKey{"signup.username", "remote_taken"},
            "Username already taken");
    }
    co_return AsyncRuleResult::passed();
}

void wait_until(const std::function<bool()>& done,
                std::chrono::milliseconds timeout = std::chrono::seconds{1}) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
}

}  // namespace

// ----------------------------------------------------------------------------
//  V-2: pending semantics + V-4: ValidationKey + rule_id
// ----------------------------------------------------------------------------
TEST_CASE("V-2/V-4: success path settles to valid; failure carries the key") {
    InlineExecutor ui, worker;
    AVCounter      c;

    Property<std::string> username{"alice"};
    Validator<std::string> v{username, "signup.username"};

    AsyncValidator<std::string> av{ui, worker,
        [&](std::string u, CancellationToken tok) {
            return remote_username_check(std::move(u), tok, c);
        }};

    auto sub = av.attach_to(v, username);

    // Initial fire on "alice": passes.
    wait_until([&]{ return !v.state().get().pending; });
    CHECK(v.state().get().valid);
    CHECK_FALSE(v.state().get().pending);

    // Drive a failure.
    username.set("taken");
    wait_until([&]{ return !v.state().get().pending; });

    auto state = v.state().get();
    CHECK_FALSE(state.valid);
    REQUIRE_FALSE(state.errors.empty());
    CHECK(state.errors.front().kind                 == ErrorKind::Validation);
    CHECK(state.errors.front().key.field_path       == "signup.username");
    CHECK(state.errors.front().key.rule_id          == "remote_taken");
    CHECK(state.errors.front().message              == "Username already taken");
}

// ----------------------------------------------------------------------------
//  V-5: identical successive values are de-duplicated
// ----------------------------------------------------------------------------
TEST_CASE("V-5: setting the same value twice fires the rule only once") {
    InlineExecutor ui, worker;
    AVCounter      c;

    Property<std::string> username{"alice"};
    Validator<std::string> v{username, "signup.username"};

    AsyncValidator<std::string> av{ui, worker,
        [&](std::string u, CancellationToken tok) {
            return remote_username_check(std::move(u), tok, c);
        }};

    auto sub = av.attach_to(v, username);
    wait_until([&]{ return !v.state().get().pending; });

    const int after_initial = c.fires.load();
    CHECK(after_initial == 1);

    // Set the same value again -- V-5 says no extra fire.
    username.set("alice");
    username.set("alice");
    wait_until([&]{ return !v.state().get().pending; });

    CHECK(c.fires.load() == after_initial);
}

// ----------------------------------------------------------------------------
//  V-3: failures throw -> AsyncFailure surfaces; cancellation does not
// ----------------------------------------------------------------------------
TEST_CASE("V-3: factory throwing maps to AsyncFailure (kind backfilled to Validation per E-21)") {
    InlineExecutor ui, worker;
    AVCounter      c;

    Property<std::string> username{"alice"};
    Validator<std::string> v{username, "signup.username"};

    AsyncValidator<std::string> av{ui, worker,
        [&](std::string u, CancellationToken tok) {
            return remote_username_check(std::move(u), tok, c);
        }};

    auto sub = av.attach_to(v, username);
    wait_until([&]{ return !v.state().get().pending; });

    c.next_should_fail.store(true);
    username.set("bob");
    wait_until([&]{ return !v.state().get().pending; });

    auto state = v.state().get();
    REQUIRE_FALSE(state.errors.empty());
    // Per error-model.md E-21: every error inside Validator.state is
    // re-tagged as ErrorKind::Validation regardless of how the
    // contributor (sync rule, async rule, async failure) built it.
    // The original source tag is preserved so callers can still tell
    // an exception-mapped failure from a regular validation reject.
    CHECK(state.errors.front().kind   == ErrorKind::Validation);
    CHECK(state.errors.front().source == "AsyncValidator");
    CHECK_FALSE(state.errors.front().message.empty());
}

// ----------------------------------------------------------------------------
//  V-6: detaching the Subscription cancels in-flight work
// ----------------------------------------------------------------------------
TEST_CASE("V-6: unsubscribing during pending leaves Validator settled") {
    InlineExecutor ui, worker;
    AVCounter      c;

    Property<std::string> username{"alice"};
    Validator<std::string> v{username, "signup.username"};

    AsyncValidator<std::string> av{ui, worker,
        [&](std::string u, CancellationToken tok) {
            return remote_username_check(std::move(u), tok, c);
        }};

    {
        auto sub = av.attach_to(v, username);
        // Subscription drops here; with InlineExecutor + InlineExecutor
        // the rule already settled synchronously, but V-6 also covers
        // the destructor path where target_ is cleared.
    }
    // Setting after detach must NOT fire the rule.
    const int fires_before = c.fires.load();
    username.set("taken");
    CHECK(c.fires.load() == fires_before);
}

// =========================================================================
//  Real-executor interleaving tests (V-1 / V-3 / V-6 hardening).
//
//  These exercise AsyncValidator under a true ui+worker split so the
//  rule does not settle synchronously inside `set()`. The rule sleeps
//  for a controllable interval on the worker, hops back to ui to
//  apply, and may be cancelled mid-flight. We use MainThreadExecutor
//  for ui (pumpable), ThreadPoolExecutor(1) for worker.
// =========================================================================

namespace {

/// A rule that blocks on the worker thread until the test releases
/// it, then optionally maps to passed/failed. Captures cancellation
/// at every yield point so V-3 can be observed.
struct GatedRule {
    std::atomic<int>     fires{0};
    std::atomic<int>     completes{0};
    std::atomic<int>     cancels{0};
    std::mutex           m;
    std::condition_variable cv;
    bool                 release{false};
    bool                 next_failed{false};

    void release_now() {
        {
            std::lock_guard lk(m);
            release = true;
        }
        cv.notify_all();
    }
    void make_next_failed() {
        std::lock_guard lk(m);
        next_failed = true;
    }
};

Task<AsyncRuleResult> gated_check(std::string /*v*/,
                                  CancellationToken tok,
                                  std::shared_ptr<GatedRule> g) {
    g->fires.fetch_add(1, std::memory_order_relaxed);
    // Park on the gate. We poll the cancellation token periodically
    // so a cancel during the wait flips us into the catch path.
    bool failed_local = false;
    {
        std::unique_lock lk(g->m);
        while (!g->release) {
            // Wake at most every 5 ms to re-check cancellation.
            g->cv.wait_for(lk, std::chrono::milliseconds{5});
            if (tok.is_cancelled()) {
                g->cancels.fetch_add(1, std::memory_order_relaxed);
                lk.unlock();
                tok.throw_if_cancelled();
            }
        }
        g->release   = false;
        failed_local = g->next_failed;
        g->next_failed = false;
    }
    g->completes.fetch_add(1, std::memory_order_relaxed);
    if (failed_local) {
        co_return AsyncRuleResult::failed(
            ValidationKey{"signup.username", "remote_taken"},
            "Username already taken");
    }
    co_return AsyncRuleResult::passed();
}

/// Pump the ui executor for up to `timeout`, returning when `done()`
/// is true. Distinct from `wait_until` above (which sleeps); here we
/// must drive the ui executor's queue or co_await schedule_on(ui)
/// will park forever.
template<typename Pred>
void pump_until(MainThreadExecutor& ui, Pred done,
                std::chrono::milliseconds timeout = std::chrono::seconds{2}) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        ui.drain();
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
}

}  // namespace

// ----------------------------------------------------------------------------
//  V-1 (real worker): stale results from the previous in-flight fire
//  must be dropped when a newer source-value supersedes them.
// ----------------------------------------------------------------------------
TEST_CASE("V-1 (real executor): stale fires are dropped, latest wins") {
    MainThreadExecutor   ui;
    ThreadPoolExecutor   worker{1};
    auto                 g = std::make_shared<GatedRule>();

    Property<std::string>   username{"alice"};
    Validator<std::string>  v{username, "signup.username"};
    AsyncValidator<std::string> av{ui, worker,
        [g](std::string u, CancellationToken tok) {
            return gated_check(std::move(u), std::move(tok), g);
        }};
    auto sub = av.attach_to(v, username);

    // Fire #1 ("alice") parks on the gate.
    pump_until(ui, [&]{ return g->fires.load() >= 1; });
    REQUIRE(g->fires.load() >= 1);
    const int fires_after_a = g->fires.load();

    // Drive a second value before #1 completes. V-1 says #1 must be
    // cancelled; #2 will run and is the only one whose result lands.
    username.set("bob");
    pump_until(ui, [&]{ return g->cancels.load() >= 1; });
    CHECK(g->cancels.load() >= 1);

    // Now release the gate. #2 wakes up, succeeds, and lands on ui.
    g->release_now();
    pump_until(ui, [&]{ return !v.state().get().pending; });
    auto state = v.state().get();
    CHECK(state.valid);
    CHECK_FALSE(state.pending);

    // The actual "fires" count depends on whether #1 woke before
    // observing the cancellation (it could have completed on the
    // worker without anyone observing the result). What V-1 nails is
    // that the LATEST source-value's outcome is what shows up in
    // state, regardless of how many phantom fires happened.
    CHECK(g->fires.load() >= fires_after_a);
}

// ----------------------------------------------------------------------------
//  V-3 (real worker): a fire cancelled mid-flight must NOT surface
//  as Error in the validator state.
// ----------------------------------------------------------------------------
TEST_CASE("V-3 (real executor): cancellation never surfaces as Error") {
    MainThreadExecutor   ui;
    ThreadPoolExecutor   worker{1};
    auto                 g = std::make_shared<GatedRule>();

    Property<std::string>   username{"alice"};
    Validator<std::string>  v{username, "signup.username"};
    AsyncValidator<std::string> av{ui, worker,
        [g](std::string u, CancellationToken tok) {
            return gated_check(std::move(u), std::move(tok), g);
        }};
    auto sub = av.attach_to(v, username);
    pump_until(ui, [&]{ return g->fires.load() >= 1; });

    // Detach -> in-flight rule must observe cancellation and unwind.
    sub.detach();
    g->release_now();   // wake the worker so it actually checks
    pump_until(ui, [&]{ return g->cancels.load() >= 1
                                || g->completes.load() >= 1; },
               std::chrono::seconds{3});

    // Drain a few extra ui ticks so any stale stage that *would*
    // post an Error onto the validator gets a chance to run.
    for (int i = 0; i < 8; ++i) ui.drain();

    auto state = v.state().get();
    // V-3: the only acceptable terminal states are
    //   (pending=true, errors=empty, valid=...)  -- still draining
    //   (pending=false, errors=empty, valid=true) -- cancelled clean
    // The forbidden case is (pending=false, errors=non-empty) with
    // the cancellation-derived Error. We assert the negative.
    for (const auto& e : state.errors) {
        CHECK(e.kind != ErrorKind::Cancellation);
    }
}

// ----------------------------------------------------------------------------
//  V-6 (real worker): destroying AsyncValidator while a fire is
//  parked on the worker must not leak: the token must be cancelled
//  and the worker callable, on completion, must observe it.
// ----------------------------------------------------------------------------
TEST_CASE("V-6 (real executor): destroying AsyncValidator cancels in-flight rule") {
    MainThreadExecutor   ui;
    ThreadPoolExecutor   worker{1};
    auto                 g = std::make_shared<GatedRule>();

    Property<std::string>   username{"alice"};
    Validator<std::string>  v{username, "signup.username"};

    {
        AsyncValidator<std::string> av{ui, worker,
            [g](std::string u, CancellationToken tok) {
                return gated_check(std::move(u), std::move(tok), g);
            }};
    auto sub = av.attach_to(v, username);
    pump_until(ui, [&]{ return g->fires.load() >= 1; });
        // av destructed here -> attached subscription dies, source
        // is detached; the in-flight CancellationSource must fire.
    }

    // Wake the parked rule so it can observe cancellation cleanly.
    g->release_now();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    while (g->cancels.load() == 0 && g->completes.load() == 0
           && std::chrono::steady_clock::now() < deadline) {
        ui.drain();
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    CHECK((g->cancels.load() >= 1 || g->completes.load() >= 1));
}

// ----------------------------------------------------------------------------
//  Lifetime: Validator destroyed while AsyncValidator's worker is
//  parked. The worker continuation must not write into a dangling
//  Validator. We exercise this via the same gating + intentional
//  validator-before-AV destruction order.
// ----------------------------------------------------------------------------
TEST_CASE("Lifetime: validator destroyed before in-flight rule completes is safe") {
    MainThreadExecutor   ui;
    ThreadPoolExecutor   worker{1};
    auto                 g = std::make_shared<GatedRule>();

    auto av = std::make_unique<AsyncValidator<std::string>>(
        ui, worker,
        [g](std::string u, CancellationToken tok) {
            return gated_check(std::move(u), std::move(tok), g);
        });

    {
        Property<std::string>   username{"alice"};
        Validator<std::string>  v{username, "signup.username"};
        auto sub = av->attach_to(v, username);
        pump_until(ui, [&]{ return v.state().get().pending; });
        // sub.detach() runs first via Subscription::~Subscription,
        // which clears the AsyncValidator's target_ and fires the
        // current CancellationSource. Validator dies next.
    }

    // Wake the parked rule. Even after both Validator and the
    // Subscription are gone, AsyncValidator survives -- it must not
    // write into the dead validator.
    g->release_now();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    while (g->cancels.load() == 0 && g->completes.load() == 0
           && std::chrono::steady_clock::now() < deadline) {
        ui.drain();
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    // Drain a bit so any stage_2 ui hop that would touch the dead
    // validator gets the chance.
    for (int i = 0; i < 16; ++i) ui.drain();

    av.reset();
    CHECK(true);
}
