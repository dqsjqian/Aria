// ============================================================================
//  test_diagnostics.cpp
// ----------------------------------------------------------------------------
//  Pin down the unified diagnostic protocol contracts spelled out in
//  docs/diagnostics.md (D-N). Each TEST_CASE references its canonical
//  invariant ID so a failure points the reader straight at the
//  authoritative description.
// ============================================================================

#include <doctest/doctest.h>

#include "aria/aria.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace aria;

namespace {

/// Tiny helper to capture every published TraceEvent into a vector.
/// Returns a movable lambda so it can be installed via ScopedTraceSink.
struct LogSink {
    std::vector<TraceEvent>* dst;

    void operator()(const TraceEvent& ev) const {
        dst->push_back(ev);
    }
};

/// Count events of a given category in the captured log.
[[nodiscard]] std::size_t count_category(
    const std::vector<TraceEvent>& log, TraceCategory cat) {
    return static_cast<std::size_t>(std::count_if(
        log.begin(), log.end(),
        [cat](const TraceEvent& ev) { return ev.category == cat; }));
}

/// Find first payload of given category whose `op` matches.
/// Returns a pointer into the captured event's payload variant; nullptr
/// if not found. The payload type must expose an `op` member of
/// std::string-like type (Async / Binding / Command / Validation /
/// List). For `trace::Reactive` use `find_reactive_phase` below.
template<class PayloadT>
[[nodiscard]] const PayloadT* find_by_op(
    const std::vector<TraceEvent>& log,
    TraceCategory cat,
    std::string_view op) {
    for (const auto& ev : log) {
        if (ev.category != cat) continue;
        if (auto* p = std::get_if<PayloadT>(&ev.payload)) {
            if (p->op == op) return p;
        }
    }
    return nullptr;
}

/// Find first reactive event with given phase. Returns a pointer into
/// the variant payload; nullptr if not seen.
[[nodiscard]] const trace::Reactive* find_reactive_phase(
    const std::vector<TraceEvent>& log, trace::ReactivePhase phase) {
    for (const auto& ev : log) {
        if (ev.category != TraceCategory::Reactive) continue;
        if (auto* p = std::get_if<trace::Reactive>(&ev.payload)) {
            if (p->phase == phase) return p;
        }
    }
    return nullptr;
}

}  // namespace

// ============================================================================
//  D-1: zero-cost when no sink is installed
// ============================================================================

TEST_CASE("D-1: has_trace_sink is false by default") {
    // No ScopedTraceSink, no install_trace_sink in this case.
    CHECK_FALSE(has_trace_sink());
}

TEST_CASE("D-1: publish_trace is a no-op when no sink is installed") {
    // We cannot directly observe the no-op; we exercise it to make sure
    // it does not throw / does not crash. Real zero-cost is enforced
    // by the implementation: a single shared_ptr load + null check.
    publish_trace(TraceCategory::Reactive,
                  trace::Reactive{trace::ReactivePhase::FlushBegin, "", 0, false});
    CHECK_FALSE(has_trace_sink());
}

// ============================================================================
//  D-2: ScopedTraceSink installs / restores
// ============================================================================

TEST_CASE("D-2: ScopedTraceSink installs for the scope and restores on exit") {
    CHECK_FALSE(has_trace_sink());
    {
        std::vector<TraceEvent> log;
        ScopedTraceSink guard{LogSink{&log}};
        CHECK(has_trace_sink());
        publish_trace(TraceCategory::Command, trace::Command{"execute"});
        REQUIRE(log.size() == 1);
        CHECK(log[0].category == TraceCategory::Command);
    }
    CHECK_FALSE(has_trace_sink());
}

TEST_CASE("D-2: nested ScopedTraceSink stacks correctly") {
    std::vector<TraceEvent> outer_log;
    ScopedTraceSink outer{LogSink{&outer_log}};

    publish_trace(TraceCategory::Command, trace::Command{"outer-1"});
    REQUIRE(outer_log.size() == 1);

    {
        std::vector<TraceEvent> inner_log;
        ScopedTraceSink inner{LogSink{&inner_log}};

        publish_trace(TraceCategory::Command, trace::Command{"inner-1"});
        // Inner sink takes over; outer sees nothing for the duration.
        CHECK(inner_log.size() == 1);
        CHECK(outer_log.size() == 1);
    }

    publish_trace(TraceCategory::Command, trace::Command{"outer-2"});
    // Outer is restored; should see exactly the post-restore event.
    CHECK(outer_log.size() == 2);
}

TEST_CASE("D-2: install_trace_sink({}) clears the active sink") {
    std::vector<TraceEvent> log;
    install_trace_sink(LogSink{&log});
    CHECK(has_trace_sink());
    publish_trace(TraceCategory::Command, trace::Command{"x"});
    CHECK(log.size() == 1);

    clear_trace_sink();
    CHECK_FALSE(has_trace_sink());
    publish_trace(TraceCategory::Command, trace::Command{"y"});
    CHECK(log.size() == 1);   // post-clear publish swallowed
}

// ============================================================================
//  Per-subsystem hookup smoke tests
// ============================================================================

TEST_CASE("Reactive: flush emits FlushBegin / Pull / Recomputed / FlushEnd") {
    Property<int> a{0};
    Computed<int> doubled{[&]{ return a.get() * 2; }};

    // Force the initial dependency edge to register, then start
    // tracing so we only see the second flush.
    (void)doubled.get();
    int probe = 0;
    auto sub = doubled.on_changed([&](int v) { probe = v; });

    std::vector<TraceEvent> log;
    ScopedTraceSink guard{LogSink{&log}};

    a.set(5);
    CHECK(probe == 10);

    // We expect at least: FlushBegin, RoundBegin, Pull, Recomputed,
    // RoundEnd, FlushEnd (in that order, possibly with other rounds
    // sandwiched).
    auto reactive = count_category(log, TraceCategory::Reactive);
    CHECK(reactive >= 6);

    // We expect at least: FlushBegin, RoundBegin, Pull, Recomputed,
    // RoundEnd, FlushEnd (in that order, possibly with other rounds
    // sandwiched). Use the phase enum to find boundary events:
    auto* begin = find_reactive_phase(log, trace::ReactivePhase::FlushBegin);
    auto* end   = find_reactive_phase(log, trace::ReactivePhase::FlushEnd);
    CHECK(begin != nullptr);
    CHECK(end   != nullptr);

    bool saw_recomp_changed = false;
    for (const auto& ev : log) {
        if (ev.category != TraceCategory::Reactive) continue;
        const auto& r = std::get<trace::Reactive>(ev.payload);
        if (r.phase == trace::ReactivePhase::Recomputed && r.changed) {
            saw_recomp_changed = true;
        }
    }
    CHECK(saw_recomp_changed);
}

TEST_CASE("List: emits Insert/Remove/Replace/Reset events with size_after") {
    std::vector<TraceEvent> log;
    ScopedTraceSink guard{LogSink{&log}};

    ObservableList<int> list;
    list.push_back(std::make_shared<int>(10));
    list.push_back(std::make_shared<int>(20));
    list.replace_at(0, std::make_shared<int>(11));
    list.remove_at(1);
    list.clear();

    // Snapshot the list-category events.
    std::vector<trace::List> list_events;
    for (const auto& ev : log) {
        if (ev.category != TraceCategory::List) continue;
        list_events.push_back(std::get<trace::List>(ev.payload));
    }
    REQUIRE(list_events.size() == 5);
    CHECK(list_events[0].op == "Insert");
    CHECK(list_events[0].size_after == 1);
    CHECK(list_events[1].op == "Insert");
    CHECK(list_events[1].size_after == 2);
    CHECK(list_events[2].op == "Replace");
    CHECK(list_events[2].size_after == 2);
    CHECK(list_events[3].op == "Remove");
    CHECK(list_events[3].size_after == 1);
    CHECK(list_events[4].op == "Reset");
    CHECK(list_events[4].size_after == 0);
}

TEST_CASE("Command: execute fires `execute`; rejected fires `rejected_can_execute`") {
    std::vector<TraceEvent> log;
    ScopedTraceSink guard{LogSink{&log}};

    Property<bool> can{true};
    int hits = 0;
    Command<> cmd{[&]{ ++hits; }, [&]{ return can.get(); }};

    cmd.execute();
    CHECK(hits == 1);

    can = false;
    cmd.execute();
    CHECK(hits == 1);   // gated

    auto* exec_ev   = find_by_op<trace::Command>(log, TraceCategory::Command, "execute");
    auto* reject_ev = find_by_op<trace::Command>(log, TraceCategory::Command,
                                                 "rejected_can_execute");
    CHECK(exec_ev   != nullptr);
    CHECK(reject_ev != nullptr);
}

TEST_CASE("Validation: rule_pass / rule_fail trace events fire on every run") {
    std::vector<TraceEvent> log;
    ScopedTraceSink guard{LogSink{&log}};

    Property<std::string> name(std::string{});
    Validator<std::string> v(name, "user.name");
    v.must([](const std::string& s) { return !s.empty(); }, "required", "required");

    // The Validator already ran once during construction. Force at
    // least one re-run by mutating the source.
    name = "alice";

    auto* pass = find_by_op<trace::Validation>(log, TraceCategory::Validation,
                                               "rule_pass");
    auto* fail = find_by_op<trace::Validation>(log, TraceCategory::Validation,
                                               "rule_fail");
    REQUIRE(pass != nullptr);
    REQUIRE(fail != nullptr);
    CHECK(pass->key.field_path == "user.name");
    CHECK(pass->key.rule_id    == "required");
    CHECK(fail->key.field_path == "user.name");
    CHECK(fail->key.rule_id    == "required");
    CHECK(fail->message        == "required");
}

// ============================================================================
//  Sink robustness
// ============================================================================

TEST_CASE("Sink: throwing handlers do not propagate") {
    ScopedTraceSink guard{[](const TraceEvent&) {
        throw std::runtime_error("sink boom");
    }};
    // If the throw escaped, this test would terminate. Reaching the
    // CHECK proves publish_trace swallowed the exception.
    publish_trace(TraceCategory::Command, trace::Command{"execute"});
    CHECK(has_trace_sink());
}
