// test_binding_dispatcher.cpp — covers the dispatcher / DispatchPolicy
// extension to BindingEngine. The four cases pin the four new code paths:
//
//   1. Direct (default)               — no marshalling, even with a dispatcher
//   2. SmartMarshal + main thread     — call inline, do NOT enqueue
//   3. SmartMarshal + worker thread   — enqueue; visible only after pump()
//   4. AlwaysPost                     — enqueue even on the main thread
//   5. View destroyed in flight       — posted lambda must not deref dead view
//
// The fake dispatcher is a thin in-test shim — we deliberately do NOT use
// SimpleDispatcher here so the test can drive `is_main_thread()` directly
// and pump the queue on demand.

#include <doctest/doctest.h>

#include "aria/binding/binding_engine.hpp"
#include "aria/runtime/dispatcher.hpp"
#include "fake_adapter.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

using namespace aria;
using namespace aria::binding;
using namespace aria::binding::testing;

namespace {

/// Test dispatcher: queues posted callables and lets the test decide when
/// to pump them. `is_main_thread()` is a settable atomic so we can simulate
/// "we are on the worker thread" without actually crossing threads in the
/// SmartMarshal-inline-fast-path test.
class FakeDispatcher final : public runtime::IDispatcher {
public:
    void post(std::function<void()> fn) override {
        std::lock_guard lk(mu_);
        queue_.push(std::move(fn));
        ++posted_;
    }
    void post_delayed(std::chrono::milliseconds, std::function<void()> fn) override {
        post(std::move(fn));
    }
    [[nodiscard]] bool is_main_thread() const noexcept override {
        return is_main_.load(std::memory_order_acquire);
    }

    void set_is_main(bool b) { is_main_.store(b, std::memory_order_release); }

    /// Pump every queued callable. Returns count processed.
    std::size_t pump() {
        std::size_t n = 0;
        for (;;) {
            std::function<void()> fn;
            {
                std::lock_guard lk(mu_);
                if (queue_.empty()) break;
                fn = std::move(queue_.front());
                queue_.pop();
            }
            fn();
            ++n;
        }
        return n;
    }

    [[nodiscard]] std::size_t posted_count() const noexcept { return posted_; }
    [[nodiscard]] std::size_t queued() const {
        std::lock_guard lk(mu_);
        return queue_.size();
    }

private:
    mutable std::mutex                 mu_;
    std::queue<std::function<void()>>  queue_;
    std::atomic<bool>                  is_main_{true};
    std::atomic<std::size_t>           posted_{0};
};

}  // namespace

TEST_CASE("BindingEngine: Direct policy never marshals") {
    auto adapter    = std::make_shared<FakeAdapter>();
    auto dispatcher = std::make_shared<FakeDispatcher>();
    BindingEngine engine(adapter, dispatcher,
                         BindingEngine::DispatchPolicy::Direct);

    Property<std::string> name("Alice");
    FakeView view;
    engine.bind_text_oneway(name, view);

    CHECK(view.text == "Alice");
    CHECK(dispatcher->posted_count() == 0);

    name = "Bob";
    CHECK(view.text == "Bob");                 // updated INLINE
    CHECK(dispatcher->posted_count() == 0);    // NOT posted

    // Even pretending we are on a worker thread, Direct must not post.
    dispatcher->set_is_main(false);
    name = "Carol";
    CHECK(view.text == "Carol");
    CHECK(dispatcher->posted_count() == 0);
}

TEST_CASE("BindingEngine: SmartMarshal stays inline on the main thread") {
    auto adapter    = std::make_shared<FakeAdapter>();
    auto dispatcher = std::make_shared<FakeDispatcher>();
    dispatcher->set_is_main(true);
    BindingEngine engine(adapter, dispatcher,
                         BindingEngine::DispatchPolicy::SmartMarshal);

    Property<int> count(0);
    FakeView view;
    engine.bind_int_oneway(count, view);

    CHECK(view.integer == 0);

    count = 7;
    CHECK(view.integer == 7);                  // inline fast path
    CHECK(dispatcher->posted_count() == 0);    // never enqueued
}

TEST_CASE("BindingEngine: SmartMarshal posts when off the main thread") {
    auto adapter    = std::make_shared<FakeAdapter>();
    auto dispatcher = std::make_shared<FakeDispatcher>();
    dispatcher->set_is_main(true);
    BindingEngine engine(adapter, dispatcher,
                         BindingEngine::DispatchPolicy::SmartMarshal);

    Property<std::string> status("idle");
    FakeView view;
    engine.bind_text_oneway(status, view);
    CHECK(view.text == "idle");

    // Simulate that a Property::set() is happening on a worker thread:
    // is_main_thread() reports false, so the engine MUST post.
    dispatcher->set_is_main(false);
    status = "running";

    CHECK(view.text == "idle");                // not yet visible
    CHECK(dispatcher->queued() == 1);          // queued for the UI thread

    // Pretend the run loop pumped the main thread.
    dispatcher->set_is_main(true);
    auto n = dispatcher->pump();
    CHECK(n == 1);
    CHECK(view.text == "running");
}

TEST_CASE("BindingEngine: AlwaysPost posts even from the main thread") {
    auto adapter    = std::make_shared<FakeAdapter>();
    auto dispatcher = std::make_shared<FakeDispatcher>();
    dispatcher->set_is_main(true);
    BindingEngine engine(adapter, dispatcher,
                         BindingEngine::DispatchPolicy::AlwaysPost);

    Property<bool> on(false);
    FakeView view;
    engine.bind_bool_oneway(on, view);
    CHECK_FALSE(view.flag);

    on = true;

    CHECK_FALSE(view.flag);                    // posted, not yet pumped
    CHECK(dispatcher->queued() == 1);

    dispatcher->pump();
    CHECK(view.flag);
}

TEST_CASE("BindingEngine: view destroyed in flight is dropped silently") {
    auto adapter    = std::make_shared<FakeAdapter>();
    auto dispatcher = std::make_shared<FakeDispatcher>();
    dispatcher->set_is_main(true);
    BindingEngine engine(adapter, dispatcher,
                         BindingEngine::DispatchPolicy::AlwaysPost);

    Property<int> n(0);
    auto view = std::make_unique<FakeView>();
    engine.bind_int_oneway(n, *view);

    n = 42;                                    // VM→View posted
    CHECK(view->integer == 0);                 // not yet pumped
    CHECK(dispatcher->queued() == 1);

    // Destroy the view BEFORE the dispatcher pumps.
    view.reset();

    // Pump must not crash and must NOT touch the (now-dead) view.
    // We can only verify the negative: the queued callable has been
    // drained without throwing or calling into the old adapter setter.
    auto processed = dispatcher->pump();
    CHECK(processed == 1);                     // we did dequeue 1 lambda
    // No further assertion needed — the test passes by not aborting.
}

TEST_CASE("BindingEngine: two-way feedback guard survives marshalling") {
    // Reproduce the converter feedback loop scenario, but with AlwaysPost:
    // the GuardFlag captured by shared_ptr must keep working across the
    // post/pump boundary so the view-echo from the adapter setter does
    // not snap the model.
    auto adapter    = std::make_shared<FakeAdapter>();
    auto dispatcher = std::make_shared<FakeDispatcher>();
    dispatcher->set_is_main(true);
    BindingEngine engine(adapter, dispatcher,
                         BindingEngine::DispatchPolicy::AlwaysPost);

    Property<std::string> name("Alice");
    FakeView view;
    engine.bind_text(name, view);

    // Initial sync ran inline (constructor is documented main-thread).
    CHECK(view.text == "Alice");

    // Counter for VM-side writes; should remain == 1 after the round-trip.
    int model_writes = 0;
    auto sub = name.on_changed([&](const std::string&) { ++model_writes; });

    // VM → View. Setter is now QUEUED, not applied.
    name = "Bob";
    CHECK(model_writes == 1);                  // our own write
    CHECK(view.text == "Alice");               // not yet applied
    CHECK(dispatcher->queued() == 1);

    // Pump: setter runs under GuardFlag, view emits sig_text, the
    // View→VM sub sees `*guard == true` and skips. The model must NOT
    // see a second write.
    dispatcher->pump();
    CHECK(view.text == "Bob");
    CHECK(model_writes == 1);                  // guard suppressed the echo
    CHECK(name.get() == "Bob");
}

TEST_CASE("BindingEngine: clear() drops in-flight VM->View callables") {
    // Regression: clear() must invalidate every queued VM→View lambda
    // that the dispatcher has not pumped yet. This is enforced via the
    // per-view alive sentinel — clear() releases the sentinel map, so
    // every posted lambda's weak_ptr<int> lock fails on pump.
    auto adapter    = std::make_shared<FakeAdapter>();
    auto dispatcher = std::make_shared<FakeDispatcher>();
    dispatcher->set_is_main(true);
    BindingEngine engine(adapter, dispatcher,
                         BindingEngine::DispatchPolicy::AlwaysPost);

    Property<std::string> status("idle");
    FakeView view;
    engine.bind_text_oneway(status, view);
    CHECK(view.text == "idle");

    // Queue a VM→View update.
    status = "running";
    CHECK(view.text == "idle");                 // not yet pumped
    CHECK(dispatcher->queued() == 1);

    // Tear down every binding while the lambda is still in flight.
    engine.clear();

    // Pump: the queued lambda must be drained but the view must NOT
    // be touched — its alive sentinel is gone, so the lambda no-ops.
    auto processed = dispatcher->pump();
    CHECK(processed == 1);
    CHECK(view.text == "idle");                 // stale write was dropped
}

TEST_CASE("BindingEngine: clear() then re-bind the same view stays clean") {
    // Regression: after clear() drops the per-view sentinel, a fresh
    // bind() on the same view must mint a brand-new sentinel; an
    // older posted lambda from before clear() must not be able to
    // resurrect the new binding by sharing its alive token.
    auto adapter    = std::make_shared<FakeAdapter>();
    auto dispatcher = std::make_shared<FakeDispatcher>();
    dispatcher->set_is_main(true);
    BindingEngine engine(adapter, dispatcher,
                         BindingEngine::DispatchPolicy::AlwaysPost);

    FakeView view;

    // Phase 1: bind, queue an update, then clear().
    Property<int> phase1(1);
    engine.bind_int_oneway(phase1, view);
    phase1 = 99;                                // queued
    CHECK(dispatcher->queued() == 1);
    engine.clear();

    // Phase 2: re-bind a new property to the same view.
    Property<int> phase2(7);
    engine.bind_int_oneway(phase2, view);
    CHECK(view.integer == 7);                   // initial sync inline

    // Pumping now must run BOTH lambdas: the stale phase-1 lambda
    // should no-op (its sentinel is dead), the phase-2 path is fine.
    auto processed = dispatcher->pump();
    CHECK(processed == 1);                      // only phase-1 was queued
    CHECK(view.integer == 7);                   // stale 99 must NOT win

    // And forward updates on phase-2 still work.
    phase2 = 11;
    CHECK(view.integer == 7);                   // queued, not pumped
    dispatcher->pump();
    CHECK(view.integer == 11);
}
