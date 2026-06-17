# Reactive Core

The reactive core is Aria's foundation. Every observable value, every derived computation, every side effect flows through a single process-wide DAG (directed acyclic graph). This chapter covers the five primitives you'll use every day:

- **`Property<T>`** — observable source node
- **`Computed<T>`** — auto-tracked derived value
- **`Effect`** — auto-tracked side-effect reaction
- **`Subscription`** — RAII handle for any observation
- **`batch()` / `untracked()`** — transaction and escape-hatch utilities

**Include:** `#include "aria/aria.hpp"` (umbrella) or `#include "aria/reactive/reactive.hpp"`

---

## Property\<T\>

`Property<T>` is the source of truth. It owns a value, notifies on change, and auto-tracks reads inside `Computed` / `Effect`.

### Basic Usage

```cpp
#include "aria/aria.hpp"

aria::Property<std::string> name{"Alice"};
aria::Property<int> age{30};
aria::Property<bool> active{true};
```

### Read

```cpp
// Auto-tracked: inside Computed/Effect, registers as dependency
std::string n = name.get();       // returns copy
const std::string& nr = name.get_ref();  // by ref, avoids copy

// Non-tracking: snapshot, never registers dependency
std::string snap = name.peek();
const std::string& snapr = name.peek_ref();

// Implicit conversion (same as get())
std::string s = name;
```

### Write

```cpp
name.set("Bob");          // equality-gated: no-op if already "Bob"
name = "Charlie";         // operator= calls set()

// Mutate-in-place: always fires (cannot detect no-op)
aria::Property<std::vector<int>> items;
items.mutate([](std::vector<int>& v) { v.push_back(42); });
```

**Equality gate:** `set(v)` where `v == current` is a no-op. Downstream observers are not notified. This eliminates redundant work automatically.

### Observe

```cpp
// on_changed: fires on every change (not on initial bind)
auto sub = name.on_changed([](const std::string& val) {
    std::cout << "name is now: " << val << "\n";
});

// bind: fires immediately with current value, then on changes
auto sub2 = age.bind([](int val) {
    std::cout << "age: " << val << "\n";  // fires NOW with 30
});

// observe: receives (old_value, new_value)
auto sub3 = age.observe([](const int& old_val, const int& new_val) {
    std::cout << "age: " << old_val << " -> " << new_val << "\n";
});
```

All three return a `Subscription`. Drop it (or call `.release()`) to stop receiving callbacks.

### Constraints

- `T` must be **copyable** and **equality-comparable** (`operator==`)
- `Property` is **non-copyable, non-movable** — its identity in the graph is tied to its address
- **Single-threaded**: all reads/writes must happen on the graph thread (debug builds assert)

---

## Computed\<T\>

`Computed<T>` derives its value from other reactive nodes. Dependencies are discovered **automatically** — every `Property::get()` or `Computed::get()` called inside the compute function becomes an upstream.

### Basic Usage

```cpp
aria::Property<double> subtotal{100.0};
aria::Property<double> tax_rate{0.08};

aria::Computed<double> total{[&] {
    return subtotal.get() * (1.0 + tax_rate.get());
}};
// total.get() == 108.0 right now

subtotal = 200.0;
// total.get() == 216.0 — recomputed automatically
```

### Conditional Dependencies

Dependencies shift dynamically based on which branches execute:

```cpp
aria::Property<bool> use_celsius{true};
aria::Property<double> celsius{25.0};
aria::Property<double> fahrenheit{77.0};

aria::Computed<double> display_temp{[&] {
    return use_celsius.get() ? celsius.get() : fahrenheit.get();
}};
// Currently depends on: use_celsius, celsius
// If use_celsius = false, next recompute depends on: use_celsius, fahrenheit
```

### Lazy & Memoized

- `get()` returns the **cached** value — no recomputation unless an upstream changed
- Recomputation only happens during a graph flush, in topological order
- If the recomputed value equals the previous one, downstream propagation stops (glitch-free)

### Observe

Same API as `Property`:

```cpp
auto sub = total.on_changed([](double val) { /* ... */ });
auto sub2 = total.bind([](double val) { /* ... */ });
auto sub3 = total.observe([](double old_v, double new_v) { /* ... */ });
```

---

## Effect

`Effect` runs a side-effect function every time any tracked read changes. Equivalent to MobX `autorun`, SolidJS `createEffect`, or Svelte 5 `$effect`.

### Basic Usage

```cpp
aria::Property<int> count{0};

aria::Effect logger{[&] {
    std::cout << "count = " << count.get() << "\n";
}};
// Prints "count = 0" immediately (eager first run)

count = 1;   // Prints "count = 1"
count = 2;   // Prints "count = 2"
```

### Stop and Restart

```cpp
aria::Effect eff{[&] { /* ... */ }};
eff.stop();          // Disconnect from graph
// ... changes to upstreams are ignored ...
// Cannot restart — create a new Effect instead
```

### Convert to Subscription

Effects can be transferred into a `SubscriptionBag`:

```cpp
aria::SubscriptionBag bag;
aria::Effect eff{[&] { /* ... */ }};
bag += std::move(eff).into_subscription();
// bag.release() or destruction disconnects the effect
```

---

## Subscription

Unified RAII handle for any observation — reactive graph, signals, event bus, all in one type.

```cpp
aria::Subscription sub = name.on_changed([](const std::string&) {});

sub.active();    // true
sub.release();   // disconnect now
sub.active();    // false
```

### SubscriptionBag

Aggregate holder — drop all subscriptions at once:

```cpp
aria::SubscriptionBag bag;

bag += name.on_changed([](const std::string&) {});
bag += age.on_changed([](int) {});
bag += total.on_changed([](double) {});

bag.clear();     // Disconnects ALL at once
bag.size();      // 0
```

In a ViewModel, `track()` does the same thing via the internal bag.

---

## batch()

Coalesce multiple writes into a single flush. Observers see one notification, not N:

```cpp
aria::Property<int> x{1};
aria::Property<int> y{2};

aria::Computed<int> sum{[&] { return x.get() + y.get(); }};

// WITHOUT batch: sum recomputes twice (glitch!)
x = 10;   // sum recompute: 10 + 2 = 12
y = 20;   // sum recompute: 10 + 20 = 30

// WITH batch: sum recomputes once
aria::batch([&] {
    x = 10;
    y = 20;
});
// sum recompute: 10 + 20 = 30 — one flush, no intermediate state
```

### BatchScope (RAII)

```cpp
{
    aria::BatchScope batch;
    x = 10;
    y = 20;
}
// Flush happens at scope exit
```

---

## untracked()

Opt out of auto-tracking inside a Computed or Effect:

```cpp
aria::Property<int> a{1};
aria::Property<int> b{2};

aria::Computed<int> c{[&] {
    // a is tracked, b is NOT
    int val = a.get();
    int snap = aria::untracked([&] { return b.get(); });
    return val + snap;
}};
// Changing b does NOT trigger recompute
// Changing a DOES trigger recompute
```

### UntrackedScope (RAII)

```cpp
aria::Computed<int> c{[&] {
    int val = a.get();
    int snap;
    {
        aria::UntrackedScope ut;
        snap = b.get();
    }
    return val + snap;
}};
```

---

## dep()

Explicit dependency hint — "treat `x` as an upstream even if I didn't read it":

```cpp
aria::Property<int> click_count{0};

aria::Effect eff{[&] {
    aria::dep(click_count);  // Depend on it without reading
    std::cout << "something happened\n";
}};
```

Rarely needed — `get()` auto-tracks in 99% of cases.

---

## CircularDependencyError

If the graph detects a cycle (A → B → A), it throws `aria::CircularDependencyError` instead of spinning infinitely. This is a hard error, not a warning — fix the cycle.

---

## Quick Reference Table

| Primitive | Kind | Produces Value | Auto-Tracked | Eager First Run |
|-----------|------|----------------|--------------|-----------------|
| `Property<T>` | Source | Yes | N/A (is source) | N/A |
| `Computed<T>` | Derivation | Yes | Yes | Yes |
| `Effect` | Reaction | No | Yes | Yes |
| `batch()` | Transaction | — | — | — |
| `untracked()` | Escape hatch | — | — | — |
| `dep()` | Hint | — | — | — |

---

## See Also

- [ViewModel →](viewmodel.md) — `Property` and `Computed` in the context of a ViewModel
- [Collections →](collections.md) — `ObservableList<T>` and derived views
- [Validation →](validation.md) — `Validator<T>` built on top of `Property`
- [Diagnostics Protocol →](../reference/diagnostics.md) — trace events from the reactive graph
