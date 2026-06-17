// inspector_demo — a hands-on tour of GraphInspector v2.
//
// The scenario: a user wires up a `formatted_price` Computed that
// reads `price` and `currency`, then complains that *changing the
// currency* fails to refire `formatted_price`. We use the flush
// tracer to show exactly why — and then fix the bug — all from a
// single binary.
//
// Run:
//   build/examples/inspector-demo/inspector_demo
//
// Output (abridged):
//
//   === Bug repro: change currency, formatted_price doesn't refire ===
//   [trace] Pull            doubled_no_currency
//   [trace] Recomputed (us=4) doubled_no_currency  changed=false
//
//   === Fix: dep on currency, then change it ===
//   [trace] Pull            with_currency
//   [trace] Recomputed (us=3) with_currency  changed=true

#include "aria/property.hpp"
#include "aria/computed.hpp"
#include "aria/reactive/inspector.hpp"

#include <iostream>
#include <string>

namespace {

const char* phase_name(aria::GraphInspector::FlushEvent::Phase p) {
    using P = aria::GraphInspector::FlushEvent::Phase;
    switch (p) {
    case P::FlushBegin:  return "FlushBegin";
    case P::RoundBegin:  return "RoundBegin";
    case P::Pull:        return "Pull";
    case P::SkipClean:   return "SkipClean";
    case P::Recomputed:  return "Recomputed";
    case P::RoundEnd:    return "RoundEnd";
    case P::FlushEnd:    return "FlushEnd";
    }
    return "?";
}

void install_pretty_tracer() {
    using namespace aria;
    GraphInspector::install_flush_tracer(
        [](const GraphInspector::FlushEvent& ev) {
            std::cout << "[trace] " << phase_name(ev.phase);
            if (ev.node) {
                std::cout << "\t" << ev.node->effective_debug_name();
            }
            if (ev.phase == GraphInspector::FlushEvent::Phase::Recomputed) {
                std::cout << "  changed=" << (ev.changed ? "true" : "false")
                          << "  duration_us=" << ev.duration_us;
            }
            std::cout << '\n';
        });
}

}  // namespace

int main() {
    using namespace aria;

    Property<double>      price(9.99);   price.set_debug_name("price");
    Property<std::string> currency("USD"); currency.set_debug_name("currency");

    // BUG: this Computed reads price but FORGETS currency.
    // When the user changes currency, no refire happens — the Computed
    // simply has no dependency on it.
    Computed<std::string> doubled_no_currency([&] {
        return std::to_string(price.get() * 2.0);
    });
    doubled_no_currency.set_debug_name("doubled_no_currency");

    // Force the initial pull so the Computed registers its (single)
    // edge to `price`.
    (void)doubled_no_currency.get();

    std::cout << "=== Bug repro: change currency, formatted_price doesn't refire ===\n";
    install_pretty_tracer();
    currency = "EUR";   // <- nothing should fire; we'll see why in the trace
    aria::GraphInspector::clear_flush_tracer();

    // FIX: read currency too. Now the Computed has both edges and
    // changing either source triggers a recompute.
    Computed<std::string> with_currency([&] {
        return std::to_string(price.get() * 2.0) + " " + currency.get();
    });
    with_currency.set_debug_name("with_currency");
    (void)with_currency.get();

    std::cout << "\n=== Fix: dep on currency, then change it ===\n";
    install_pretty_tracer();
    currency = "JPY";
    aria::GraphInspector::clear_flush_tracer();

    std::cout << "\n=== Reachable graph (text dump) ===\n";
    std::cout << aria::GraphInspector::to_text(
        {&price, &currency, &doubled_no_currency, &with_currency});

    return 0;
}
