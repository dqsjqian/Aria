// test_binding_dispatcher.cpp — covers the dispatcher / DispatchPolicy
// extension to BindingEngine in both binding directions:
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
/// "we are on the worker thread" without actually crossing threads. Real
/// worker threads always report false, allowing concurrency regressions
/// to pump on the owner while a worker callback is still running.
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
        return std::this_thread::get_id() == owner_thread_ &&
               is_main_.load(std::memory_order_acquire);
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
    const std::thread::id              owner_thread_ = std::this_thread::get_id();
    mutable std::mutex                 mu_;
    std::queue<std::function<void()>>  queue_;
    std::atomic<bool>                  is_main_{true};
    std::atomic<std::size_t>           posted_{0};
};

// Models an adapter worker that copied a callback before disconnect.
// Those copies may still arrive after clear, view destruction or engine
// destruction, even though the adapter subscription has been released.
class CapturingAdapter final : public FakeAdapter {
public:
    void set_text(IView& view, std::string_view value) override {
        FakeAdapter::set_text(view, value);
        if (auto callback = std::exchange(during_set_text, {})) callback();
    }
    Subscription on_text_changed(
        IView& view, std::function<void(std::string_view)> cb) override {
        text_callback = cb;
        return FakeAdapter::on_text_changed(view, std::move(cb));
    }
    Subscription on_click(IView& view, std::function<void()> cb) override {
        click_callback = cb;
        return FakeAdapter::on_click(view, std::move(cb));
    }

    std::function<void(std::string_view)> text_callback;
    std::function<void()> click_callback;
    std::function<void()> during_set_text;
};

}  // namespace

TEST_CASE("BindingEngine: queued input drops a destroyed property") {
    auto adapter = std::make_shared<FakeAdapter>();
    auto dispatcher = std::make_shared<FakeDispatcher>();
    BindingEngine engine(adapter, dispatcher, BindingEngine::DispatchPolicy::AlwaysPost);
    FakeView view;
    auto property = std::make_unique<Property<int>>(0);
    engine.bind_int(*property, view);
    view.sig_int.emit(1);
    property.reset();
    CHECK(dispatcher->pump() == 1);
    view.sig_int.emit(2);
    CHECK(dispatcher->pump() == 1);
}

TEST_CASE("BindingEngine: converter clear cancels the remaining model write") {
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine(adapter);
    Property<int> property(0);
    FakeView view;
    engine.bind_int_converted(property, view, Converter<int, int>{
        [](int value) { return value; }, {},
        [&](int value) -> std::optional<int> {
            engine.clear();
            return value;
        }});
    view.sig_int.emit(1);
    CHECK(property.get() == 0);
}

TEST_CASE("BindingEngine: queued clicks drop a destroyed command") {
    auto adapter = std::make_shared<CapturingAdapter>();
    auto dispatcher = std::make_shared<FakeDispatcher>();
    BindingEngine engine(adapter, dispatcher, BindingEngine::DispatchPolicy::AlwaysPost);
    FakeView view;
    int calls = 0;
    auto command = std::make_unique<Command<>>([&] { ++calls; });
    engine.bind_command(*command, view);
    adapter->click_callback();
    command.reset();
    CHECK(dispatcher->pump() == 1);
    adapter->click_callback();
    CHECK(dispatcher->pump() == 1);
    CHECK(calls == 0);
}

TEST_CASE("BindingEngine: converter may destroy its model before returning") {
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine(adapter);
    auto property = std::make_unique<Property<int>>(0);
    FakeView view;
    engine.bind_int_converted(*property, view, Converter<int, int>{
        [](int value) { return value; }, {},
        [&](int value) -> std::optional<int> {
            property.reset();
            return value;
        }});
    view.sig_int.emit(1);
    CHECK_FALSE(property);
}

TEST_CASE("BindingEngine: binding trace can destroy the engine") {
    auto adapter = std::make_shared<FakeAdapter>();
    auto engine = std::make_unique<BindingEngine>(adapter);
    Property<int> property(0);
    FakeView view;
    engine->bind_int_oneway(property, view);
    ScopedTraceSink trace{[&](const TraceEvent& event) {
        if (event.category == TraceCategory::Binding) engine.reset();
    }};
    property.set(1);
    CHECK_FALSE(engine);
    CHECK(view.integer == 0);
}

TEST_CASE("BindingEngine: converter clear cancels the remaining view write") {
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine(adapter);
    Property<int> property(0);
    FakeView view;
    engine.bind_int_converted(property, view, Converter<int, int>{
        [&](int value) {
            if (value != 0) engine.clear();
            return value;
        }, [](int value) { return value; }, {}});
    property.set(1);
    CHECK(view.integer == 0);
}

TEST_CASE("BindingEngine: clear callbacks may clear and bind again") {
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine(adapter);
    FakeView first, second;
    Property<int> property(0);
    int called = 0;
    engine.bind_view_lifetime(first, [&] {
        ++called;
        engine.clear();
        engine.bind_int_oneway(property, second);
    });
    engine.clear();
    CHECK(called == 1);
    property.set(7);
    CHECK(second.integer == 7);
}

TEST_CASE("BindingEngine: integer conversion runs on owner thread and clear cancels queued input") {
    enum class Choice { First, Second, Third };
    auto adapter = std::make_shared<FakeAdapter>();
    auto dispatcher = std::make_shared<FakeDispatcher>();
    BindingEngine engine(adapter, dispatcher, BindingEngine::DispatchPolicy::SmartMarshal);
    const auto owner = std::this_thread::get_id();
    Property<Choice> selected(Choice::First);
    FakeView view;
    int conversions = 0;
    engine.bind_int_converted(selected, view, Converter<Choice, int>{
        [](Choice value) { return static_cast<int>(value); }, {},
        [&](int value) -> std::optional<Choice> {
            CHECK(std::this_thread::get_id() == owner);
            ++conversions;
            return static_cast<Choice>(value);
        }});
    std::thread first([&] { view.sig_int.emit(1); });
    first.join();
    CHECK(selected.get() == Choice::First);
    CHECK(conversions == 0);
    CHECK(dispatcher->pump() == 1);
    CHECK(selected.get() == Choice::Second);
    CHECK(view.integer == 1);
    CHECK(conversions == 1);

    std::thread second([&] { view.sig_int.emit(2); });
    second.join();
    engine.clear();
    CHECK(dispatcher->pump() == 1);
    CHECK(selected.get() == Choice::Second);
    CHECK(conversions == 1);
    selected.set(Choice::Third);
    CHECK(view.integer == 1);
}

TEST_CASE("BindingEngine: posted integer conversion suppresses lossy setter echoes") {
    auto adapter = std::make_shared<FakeAdapter>();
    auto dispatcher = std::make_shared<FakeDispatcher>();
    BindingEngine engine(adapter, dispatcher, BindingEngine::DispatchPolicy::AlwaysPost);
    Property<double> value(1.9);
    FakeView view;
    engine.bind_int_converted(value, view, Converter<double, int>{
        [](double x) { return static_cast<int>(x); },
        [](int x) { return static_cast<double>(x); }, {}});
    CHECK(view.integer == 1);
    CHECK(value.get() == doctest::Approx(1.9));
    value.set(2.9);
    CHECK(view.integer == 1);
    CHECK(dispatcher->pump() == 1);
    CHECK(view.integer == 2);
    CHECK(value.get() == doctest::Approx(2.9));
    CHECK(dispatcher->queued() == 0);
    adapter->set_int(view, 7);
    CHECK(value.get() == doctest::Approx(2.9));
    dispatcher->pump();
    CHECK(value.get() == doctest::Approx(7.0));
}

TEST_CASE("BindingEngine: view destruction cancels queued integer conversion") {
    auto adapter = std::make_shared<FakeAdapter>();
    auto dispatcher = std::make_shared<FakeDispatcher>();
    BindingEngine engine(adapter, dispatcher, BindingEngine::DispatchPolicy::AlwaysPost);
    Property<int> value(1);
    auto view = std::make_unique<FakeView>();
    engine.bind_int_converted(value, *view, Converter<int, int>{
        [](int x) { return x; }, [](int x) { return x; }, {}});
    value.set(2);
    view->sig_int.emit(3);
    REQUIRE(dispatcher->queued() == 2);
    view.reset();
    CHECK(dispatcher->pump() == 2);
    CHECK(value.get() == 2);
}

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
    CHECK(dispatcher->pump() == 1);           // no inbound echo task was posted
    CHECK(dispatcher->queued() == 0);
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

TEST_CASE("BindingEngine: Direct and on-thread SmartMarshal apply inbound callbacks inline") {
    auto policy = BindingEngine::DispatchPolicy::Direct;
    SUBCASE("Direct ignores dispatcher thread detection") {}
    SUBCASE("SmartMarshal on graph thread") {
        policy = BindingEngine::DispatchPolicy::SmartMarshal;
    }

    auto adapter = std::make_shared<FakeAdapter>();
    auto dispatcher = std::make_shared<FakeDispatcher>();
    BindingEngine engine(adapter, dispatcher, policy);
    Property<int> scalar(1);
    Property<int> converted(2);
    int command_result = 0;
    Command<int> command([&](int value) { command_result = value; });
    FakeView scalar_view, converted_view, button;
    engine.bind_int(scalar, scalar_view);
    engine.bind_text_converted(converted, converted_view, converters::int_to_string());
    engine.bind_command(command, button, 7);
    if (policy == BindingEngine::DispatchPolicy::Direct) dispatcher->set_is_main(false);

    scalar_view.sig_int.emit(3);
    FakeAdapter::user_type(converted_view, "4");
    FakeAdapter::user_click(button);

    CHECK(scalar.get() == 3);
    CHECK(converted.get() == 4);
    CHECK(command_result == 7);
    CHECK(dispatcher->queued() == 0);
}

TEST_CASE("BindingEngine: worker input marshals every scalar, converter and command") {
    auto adapter = std::make_shared<FakeAdapter>();
    auto dispatcher = std::make_shared<FakeDispatcher>();
    BindingEngine engine(adapter, dispatcher, BindingEngine::DispatchPolicy::SmartMarshal);
    const auto graph_thread = std::this_thread::get_id();
    Property<std::string> text("initial");
    Property<bool> flag(false);
    Property<int> integer(1);
    Property<std::int64_t> i64(2);
    Property<std::uint64_t> u64(3);
    Property<float> f32(4.0f);
    Property<double> f64(5.0);
    Property<int> converted(6);
    FakeView text_view, flag_view, int_view, i64_view, u64_view;
    FakeView float_view, double_view, converted_view, button;
    int converter_calls = 0;
    auto converter = converters::int_to_string();
    converter.try_to_model = [&](const std::string& value) -> std::optional<int> {
        CHECK(std::this_thread::get_id() == graph_thread);
        ++converter_calls;
        return std::stoi(value);
    };
    int command_result = 0;
    int predicate_calls = 0;
    Command<int> command(
        [&](int value) {
            CHECK(std::this_thread::get_id() == graph_thread);
            command_result = value;
        },
        [&](int) {
            CHECK(std::this_thread::get_id() == graph_thread);
            ++predicate_calls;
            return true;
        });
    engine.bind_text(text, text_view);
    engine.bind_bool(flag, flag_view);
    engine.bind_int(integer, int_view);
    engine.bind_int64(i64, i64_view);
    engine.bind_uint64(u64, u64_view);
    engine.bind_float(f32, float_view);
    engine.bind_double(f64, double_view);
    engine.bind_text_converted(converted, converted_view, std::move(converter));
    engine.bind_command(command, button, 99);
    predicate_calls = 0;
    auto changed = text.on_changed([&](const std::string&) {
        CHECK(std::this_thread::get_id() == graph_thread);
    });

    const std::string expected_text(512, 'x');
    std::thread worker([&] {
        std::string borrowed_text = expected_text;
        text_view.sig_text.emit(borrowed_text);
        borrowed_text.assign(512, 'y'); // invalidate the borrowed view before drain
        flag_view.sig_bool.emit(true);
        int_view.sig_int.emit(11);
        i64_view.sig_int64.emit(-1234567890123LL);
        u64_view.sig_uint64.emit(12345678901234ULL);
        float_view.sig_float.emit(1.25f);
        double_view.sig_double.emit(2.5);
        std::string borrowed_number = "42";
        converted_view.sig_text.emit(borrowed_number);
        borrowed_number.assign("88");
        FakeAdapter::user_click(button);
    });
    worker.join();

    CHECK(text.get() == "initial");
    CHECK_FALSE(flag.get());
    CHECK(integer.get() == 1);
    CHECK(i64.get() == 2);
    CHECK(u64.get() == 3);
    CHECK(f32.get() == 4.0f);
    CHECK(f64.get() == 5.0);
    CHECK(converted.get() == 6);
    CHECK(converter_calls == 0);
    CHECK(predicate_calls == 0);
    CHECK(command_result == 0);
    CHECK(dispatcher->queued() == 9);

    CHECK(dispatcher->pump() == 9);
    CHECK(text.get() == expected_text);
    CHECK(flag.get());
    CHECK(integer.get() == 11);
    CHECK(i64.get() == -1234567890123LL);
    CHECK(u64.get() == 12345678901234ULL);
    CHECK(f32.get() == 1.25f);
    CHECK(f64.get() == 2.5);
    CHECK(converted.get() == 42);
    CHECK(converter_calls == 1);
    CHECK(predicate_calls == 1);
    CHECK(command_result == 99);
}

TEST_CASE("BindingEngine: AlwaysPost queues inbound callbacks and suppresses lossy echoes") {
    auto adapter = std::make_shared<FakeAdapter>();
    auto dispatcher = std::make_shared<FakeDispatcher>();
    BindingEngine engine(adapter, dispatcher, BindingEngine::DispatchPolicy::AlwaysPost);
    Property<int> scalar(1);
    Property<double> converted(1.234);
    int commands = 0;
    Command<> command([&] { ++commands; });
    FakeView scalar_view, converted_view, button;
    engine.bind_int(scalar, scalar_view);
    engine.bind_text_converted(converted, converted_view, converters::double_to_string(1));
    engine.bind_command(command, button);

    scalar_view.sig_int.emit(7);
    FakeAdapter::user_type(converted_view, "4.567");
    FakeAdapter::user_click(button);
    CHECK(scalar.get() == 1);
    CHECK(converted.get() == doctest::Approx(1.234));
    CHECK(commands == 0);
    CHECK(dispatcher->queued() == 3);

    // Three inbound tasks and two outbound updates. Setter echoes must
    // not post additional work after their synchronous guard goes away.
    CHECK(dispatcher->pump() == 5);
    CHECK(scalar.get() == 7);
    CHECK(converted.get() == doctest::Approx(4.567));
    CHECK(converted_view.text == "4.6");
    CHECK(commands == 1);
    CHECK(dispatcher->posted_count() == 5);
}

TEST_CASE("BindingEngine: worker input during a guarded setter is retained") {
    auto adapter = std::make_shared<CapturingAdapter>();
    auto dispatcher = std::make_shared<FakeDispatcher>();
    BindingEngine engine(adapter, dispatcher, BindingEngine::DispatchPolicy::AlwaysPost);
    Property<std::string> text("initial");
    FakeView view;
    engine.bind_text(text, view);
    adapter->during_set_text = [&] {
        // The graph thread's guard is true while this worker delivers
        // an independent user edit. It must queue without reading it.
        std::thread worker([callback = adapter->text_callback] {
            callback("worker edit");
        });
        worker.join();
    };

    text = "model update";
    CHECK(dispatcher->pump() == 3);
    CHECK(text.get() == "worker edit");
    CHECK(view.text == "worker edit");
}

TEST_CASE("BindingEngine: stale inbound callbacks cannot reach destroyed model objects") {
    auto adapter = std::make_shared<CapturingAdapter>();
    auto dispatcher = std::make_shared<FakeDispatcher>();
    auto engine = std::make_unique<BindingEngine>(
        adapter, dispatcher, BindingEngine::DispatchPolicy::AlwaysPost);
    auto view = std::make_unique<FakeView>();
    auto property = std::make_unique<Property<std::string>>("initial");
    int commands = 0;
    auto command = std::make_unique<Command<>>([&] { ++commands; });
    engine->bind_text(*property, *view);
    engine->bind_command(*command, *view);
    const auto old_text = adapter->text_callback;
    const auto old_click = adapter->click_callback;
    old_text("queued before teardown");
    old_click();

    SUBCASE("clear") { engine->clear(); }
    SUBCASE("view destruction") { view.reset(); }
    SUBCASE("engine destruction") { engine.reset(); }
    property.reset();
    command.reset();

    // Simulate an HTTP worker that copied these callbacks before their
    // subscriptions were released, then invokes them after teardown.
    std::thread worker([&] {
        old_text("delivered after teardown");
        old_click();
    });
    worker.join();
    CHECK(dispatcher->pump() == 4);
    CHECK(commands == 0);
}

TEST_CASE("BindingEngine: clear and rebind gives inbound callbacks a fresh token") {
    auto adapter = std::make_shared<CapturingAdapter>();
    auto dispatcher = std::make_shared<FakeDispatcher>();
    BindingEngine engine(adapter, dispatcher, BindingEngine::DispatchPolicy::AlwaysPost);
    FakeView view;
    Property<int> old_value(1), new_value(2);
    int old_commands = 0;
    int new_commands = 0;
    Command<> old_command([&] { ++old_commands; });
    Command<> new_command([&] { ++new_commands; });
    engine.bind_text_converted(old_value, view, converters::int_to_string());
    engine.bind_command(old_command, view);
    const auto old_text = adapter->text_callback;
    const auto old_click = adapter->click_callback;
    old_text("99");
    old_click();
    engine.clear();

    engine.bind_text_converted(new_value, view, converters::int_to_string());
    engine.bind_command(new_command, view);
    old_text("88");
    old_click();
    adapter->text_callback("7");
    adapter->click_callback();
    CHECK(dispatcher->pump() == 7); // six inbound tasks, one live outbound update
    CHECK(old_value.get() == 1);
    CHECK(new_value.get() == 7);
    CHECK(old_commands == 0);
    CHECK(new_commands == 1);
    CHECK(view.text == "7");
}
