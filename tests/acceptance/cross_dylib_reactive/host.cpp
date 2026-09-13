#include "bridge.hpp"
#include <aria/diagnostics.hpp>
#include <aria/property.hpp>
#include <aria/property_ops.hpp>
#include <cstdio>
#include <memory>

namespace {
struct Timer final : aria::IDelayedScheduler {
    void post_after(std::chrono::milliseconds, std::function<void()> callback) override {
        callback();
    }
};
}

int main() {
    if (aria_test_graph_address() != &aria::reactive::Node::graph()) {
        std::fputs("host and plugin use different reactive graphs\n", stderr);
        return 1;
    }
    aria::Property<int> source{2};
    if (aria_test_flush_hook_address() != &aria::reactive::flush_trace_hook_()) {
        std::fputs("host and plugin use different flush tracer storage\n", stderr);
        return 8;
    }
    if (source.effective_debug_name() == aria_test_source_label()) {
        std::fputs("host and plugin assigned the same node identity\n", stderr);
        return 9;
    }
    int value = 0;
    int changes = 0;
    std::unique_ptr<void, decltype(&aria_test_projection_destroy)> projection{
        aria_test_projection_create(&source, &value, &changes), &aria_test_projection_destroy};
    if (value != 6 || changes != 0) return 2;
    source.set(3);
    if (value != 9 || changes != 1) return 3;
    aria::batch([&] { source.set(4); source.set(5); });
    if (value != 15 || changes != 2) return 4;
    projection.reset();
    source.set(6);
    if (changes != 2) return 5;

    int traces = 0;
    aria::ScopedTraceSink sink{[&](const aria::TraceEvent& event) {
        if (event.category == aria::TraceCategory::Command) ++traces;
    }};
    aria_test_publish_trace();
    if (traces != 1) return 6;
    // A core-only consumer must resolve its delayed scheduler base as well.
    Timer timer;
    bool ran = false;
    timer.schedule([&] { ran = true; });
    if (!ran) return 7;
    std::puts("core-only cross-library graph, diagnostics, and scheduler passed");
}
