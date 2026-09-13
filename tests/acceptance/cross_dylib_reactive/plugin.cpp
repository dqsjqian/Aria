#include "bridge.hpp"
#include <aria/computed.hpp>
#include <aria/diagnostics.hpp>
#include <aria/i_property.hpp>

namespace {
struct Projection {
    aria::Computed<int> value;
    aria::Subscription subscription;
    Projection(aria::IProperty& source, int& output, int& changes)
        : value([&source] { return std::any_cast<int>(source.get_any()) * 3; }) {
        output = value.get();
        subscription = value.on_changed([&output, &changes](int next) {
            output = next;
            ++changes;
        });
    }
};
}

extern "C" ARIA_TEST_BRIDGE_API void* aria_test_graph_address() {
    return &aria::reactive::Node::graph();
}
extern "C" ARIA_TEST_BRIDGE_API void*
aria_test_projection_create(aria::IProperty* source, int* value, int* changes) {
    return new Projection{*source, *value, *changes};
}
extern "C" ARIA_TEST_BRIDGE_API void aria_test_projection_destroy(void* projection) {
    delete static_cast<Projection*>(projection);
}
extern "C" ARIA_TEST_BRIDGE_API void aria_test_publish_trace() {
    aria::publish_trace(aria::TraceCategory::Command, aria::trace::Command{"plugin_trace"});
}

extern "C" ARIA_TEST_BRIDGE_API void* aria_test_flush_hook_address() {
    return &aria::reactive::flush_trace_hook_();
}
extern "C" ARIA_TEST_BRIDGE_API const char* aria_test_source_label() {
    static aria::Property<int> source{0};
    return source.effective_debug_name().c_str();
}
