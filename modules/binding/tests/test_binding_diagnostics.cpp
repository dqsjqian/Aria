// ============================================================================
//  test_binding_diagnostics.cpp
// ----------------------------------------------------------------------------
//  Pin down the diagnostics emissions documented in
//  docs/diagnostics.md (Binding category) -- BindingEngine publishes
//  `trace::Binding` events for VM->View / View->VM edges,
//  feedback-loop suppression and view-destroy drop. See also
//  api-style.md S-30 (one load + null check on the no-sink path) and
//  lifecycle.md L-32 (no callback into a destroyed view).
// ============================================================================

#include <doctest/doctest.h>

#include "aria/aria.hpp"
#include "aria/binding/binding_engine.hpp"

#include "fake_adapter.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace aria;
using namespace aria::binding;
using ::testing::FakeAdapter;
using ::testing::FakeView;

namespace {

[[nodiscard]] std::vector<TraceEvent>
collect_binding_events(const std::vector<TraceEvent>& log) {
    std::vector<TraceEvent> out;
    for (const auto& ev : log) {
        if (ev.category == TraceCategory::Binding) out.push_back(ev);
    }
    return out;
}

}  // namespace

TEST_CASE("Binding/diag: VM->View int write emits a Binding trace event") {
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine{adapter};

    FakeView view;
    Property<int> source{0};
    engine.bind_int_oneway(source, view);

    std::vector<TraceEvent> log;
    ScopedTraceSink guard{[&log](const TraceEvent& ev) { log.push_back(ev); }};

    source.set(7);

    auto binding_log = collect_binding_events(log);
    REQUIRE_FALSE(binding_log.empty());
    bool saw_op = false;
    for (const auto& ev : binding_log) {
        if (auto* p = std::get_if<trace::Binding>(&ev.payload)) {
            if (!p->op.empty()) { saw_op = true; break; }
        }
    }
    CHECK(saw_op);
}

TEST_CASE("Binding/diag: feedback-loop suppression keeps the trace stream finite") {
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine{adapter};

    FakeView view;
    Property<std::string> source{"hello"};
    engine.bind_text(source, view);   // two-way

    std::vector<TraceEvent> log;
    ScopedTraceSink guard{[&log](const TraceEvent& ev) { log.push_back(ev); }};

    // VM -> View, then a View -> VM echo. Feedback-loop suppression
    // must collapse the round-trip: the trace stream stays bounded
    // and never raises a BindingFailure error.
    source.set("world");
    adapter->set_text(view, "world");

    auto binding_log = collect_binding_events(log);
    REQUIRE_FALSE(binding_log.empty());
    for (const auto& ev : binding_log) {
        if (ev.error.has_value()) {
            CHECK(ev.error->kind != ErrorKind::BindingFailure);
        }
    }
    CHECK(binding_log.size() < 32);   // bounded -- no ping-pong
}

TEST_CASE("Binding/diag: ScopedTraceSink scope exit returns to no-sink mode") {
    auto adapter = std::make_shared<FakeAdapter>();
    BindingEngine engine{adapter};

    FakeView view;
    Property<int> source{0};
    engine.bind_int_oneway(source, view);

    std::vector<TraceEvent> log;
    {
        ScopedTraceSink guard{[&log](const TraceEvent& ev) { log.push_back(ev); }};
        source.set(1);
    }
    const auto count_inside_scope = log.size();

    // Outside the ScopedTraceSink, trace publishing returns to
    // no-sink mode -- subsequent writes do NOT grow the log.
    source.set(2);
    source.set(3);
    CHECK(log.size() == count_inside_scope);
}
