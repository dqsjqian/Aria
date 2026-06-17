# Diagnostics & Debugging

Aria's diagnostics system lets you observe the inner workings of the reactive graph, async commands, bindings, validators, and list mutations — all through a single, zero-overhead trace protocol.

**Include:** `#include "aria/diagnostics.hpp"`, `#include "aria/reactive/inspector.hpp"`

---

## Zero-Overhead Contract

When no trace sink is installed, the publish path costs **one atomic load + one branch**. No strings are built, no allocations happen. This means you can ship production builds with trace instrumentation compiled in and pay nothing until you attach a debugger.

---

## Trace Categories

Every trace event carries a `TraceCategory`:

| Category | Enum Value | Covers |
|----------|------------|--------|
| `Reactive` | 0 | Graph flush, push-color, pull-evaluate |
| `Async` | 1 | AsyncCommand / AsyncResource lifecycle |
| `Binding` | 2 | BindingEngine VM↔View dispatch |
| `Command` | 3 | Synchronous Command execution |
| `Validation` | 4 | Validator / FormValidator rule runs |
| `List` | 5 | ObservableList / derived list mutations |

---

## Installing a Global Trace Sink

```cpp
#include "aria/diagnostics.hpp"

aria::install_trace_sink([](const aria::TraceEvent& ev) {
    std::cout << "[" << aria::to_string(ev.category) << "] "
              << ev.debug_name << "\n";
});
```

The sink is **thread-safe** — install, replace, or clear from any thread at any time.

### Clear the Sink

```cpp
aria::clear_trace_sink();
// Publish path returns to zero-overhead
```

---

## TraceEvent Structure

```cpp
struct TraceEvent {
    TraceCategory category;
    std::string debug_name;       // node name (if set)
    std::variant<
        trace::Reactive,
        trace::Async,
        trace::Binding,
        trace::Command,
        trace::Validation,
        trace::List
    > payload;
    // + timestamp, thread_id, etc.
};
```

Each payload variant carries category-specific data:

- **`trace::Reactive`** — flush phase, node state transitions
- **`trace::Async`** — command execution start/end/error
- **`trace::Binding`** — bind/unbind events, dispatch direction
- **`trace::Command`** — execute / rejected_can_execute / can_execute_changed
- **`trace::Validation`** — rule_pass / rule_fail / begin_pending / end_pending
- **`trace::List`** — insert/remove/reset/change events

---

## GraphInspector

For deeper reactive graph introspection:

```cpp
#include "aria/reactive/inspector.hpp"

aria::GraphInspector inspector;

// Dump the graph as DOT (Graphviz)
std::string dot = inspector.to_dot();

// Dump as JSON
std::string json = inspector.to_json();

// Install a flush tracer for detailed step-by-step logging
inspector.install_flush_tracer([](const aria::GraphInspector::FlushEvent& ev) {
    std::cout << "Phase: " << ev.phase
              << " Node: " << ev.node_name
              << " Changed: " << ev.changed << "\n";
});
```

### DOT Output

Generate a visual representation of the reactive DAG:

```cpp
std::ofstream out("graph.dot");
out << aria::GraphInspector{}.to_dot();
// Render: dot -Tpng graph.dot -o graph.png
```

Nodes are labeled with their `debug_name` (if set via `set_debug_name()`). Edges show dependency direction.

---

## ScopedTraceSink

Install a trace sink for a limited scope, restoring the previous sink on exit:

```cpp
void test_reactive_flow() {
    std::vector<std::string> log;

    aria::ScopedTraceSink scoped([&](const aria::TraceEvent& ev) {
        log.push_back(ev.debug_name);
    });

    // ... exercise reactive code ...

    // Sink automatically restored (or cleared) when scoped exits
    REQUIRE(log.size() > 0);
}
```

---

## Debug Names

Set human-readable names on reactive nodes for easier debugging:

```cpp
aria::Property<int> count{0};
count.set_debug_name("counter");

aria::Computed<int> doubled{[&] { return count.get() * 2; }};
doubled.set_debug_name("counter×2");
```

These names appear in DOT graphs, JSON dumps, and trace events.

---

## Filtering by Category

```cpp
aria::install_trace_sink([](const aria::TraceEvent& ev) {
    if (ev.category != aria::TraceCategory::Validation) return;
    // Only process validation events
});
```

---

## Quick Reference

| Function / Type | Purpose |
|-----------------|---------|
| `install_trace_sink(fn)` | Set global trace consumer |
| `clear_trace_sink()` | Remove consumer, restore zero-overhead |
| `has_trace_sink()` | Check if a consumer is active |
| `ScopedTraceSink` | RAII: install + auto-restore |
| `GraphInspector::to_dot()` | Export DAG as DOT |
| `GraphInspector::to_json()` | Export DAG as JSON |
| `GraphInspector::install_flush_tracer(fn)` | Per-flush step logging |
| `Node::set_debug_name(str)` | Human-readable label |

---

## See Also

- [Diagnostics Protocol →](../reference/diagnostics.md) — authoritative contract specification
- [Reactive Core →](reactive-core.md) — the graph being inspected
- [Performance Baselines →](../reference/performance.md) — overhead measurements
