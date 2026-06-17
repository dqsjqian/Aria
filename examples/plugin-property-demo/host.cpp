// ============================================================================
//  host.cpp — the "host side" of the cross-dylib ABI smoke (ROADMAP P2-A).
// ----------------------------------------------------------------------------
//  The host instantiates the real `aria::Property<T>` objects (so the
//  reactive Graph and all template symbols live here), then hands their
//  stable `aria::IProperty` interface to a plugin compiled into a separate
//  shared library. After the plugin has driven the properties purely
//  through that interface, the host verifies the mutations actually landed
//  in its own reactive graph.
//
//  Exit code 0 = PASS. Any other code identifies which side failed (the
//  plugin's 1..11 codes, or the host's 100..102 post-conditions).
//
//  Threading: everything runs on the main thread, which is the graph
//  thread, so the IProperty calls satisfy the lifecycle contract (L-1).
// ============================================================================
#include "aria_plugin_abi.h"

// Canonical entry point: pulls the whole reactive subsystem (graph + node
// out-of-line definitions) and promotes `aria::Property<T>`. Including the
// bare `aria/reactive/property.hpp` would declare Property but leave the
// Node/Graph engine symbols undefined at link time.
#include "aria/property.hpp"

#include <cstdio>
#include <string>

int main() {
    using aria::Property;

    Property<int>         counter{0};
    Property<std::string> name{"init"};

    // Host-side observer: proves the plugin's writes propagate through the
    // host's reactive graph, not just the stored value.
    int host_changes = 0;
    auto host_sub = counter.on_changed([&host_changes](const int&) {
        ++host_changes;
    });

    // Cross the DSO boundary: the plugin sees only `aria::IProperty`.
    const int rc = aria_plugin_exercise(&counter, &name);
    if (rc != 0) {
        std::fprintf(stderr, "plugin reported ABI failure code %d\n", rc);
        return rc;
    }

    // ── Host-side post-conditions ────────────────────────────────────────
    if (counter.get() != 9) {
        std::fprintf(stderr, "counter expected 9, got %d\n", counter.get());
        return 100;
    }
    if (name.get() != "hello-from-plugin") {
        std::fprintf(stderr, "name expected 'hello-from-plugin', got '%s'\n",
                     name.get().c_str());
        return 101;
    }
    // The plugin set the int property to 42, then 7, then 9 — three actual
    // value changes from the initial 0. on_changed fires once per change.
    if (host_changes != 3) {
        std::fprintf(stderr, "host observed %d changes, expected 3\n",
                     host_changes);
        return 102;
    }

    std::puts("plugin-property-demo: cross-dylib IProperty ABI smoke PASSED");
    return 0;
}
