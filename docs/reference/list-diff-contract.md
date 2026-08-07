# Aria List Diff Contract

> This document is the framework's authoritative reference for **list
> change semantics**.
> Any type that satisfies the `aria::ListSource` concept
> (`ObservableList<T>` / `FilteredList<T>` / `SortedList<T>` /
> `MappedList<S,T>` and any future derived list) MUST emit
> `ListChange<T>` events that follow this contract.
> Any list-consuming adapter (Qt6 / AppKit / UIKit / future React
> Native / WASM / ...) MUST interpret event streams strictly per the
> rules below.
>
> Together with [`lifecycle.md`](./lifecycle.md),
> [`api-style.md`](./api-style.md) and
> [`error-model.md`](./error-model.md), this file forms the framework's
> contract document family. Every contract item is numbered `D-N` so
> that code, tests, and the CHANGELOG can reference it directly.

What a "best-in-class C++ MVVM framework" demands of its list protocol:

1. **Self-contained**: every event can be interpreted in isolation,
   without scanning history.
2. **Deterministic ordering**: the event sequence for a batched
   mutation is **fully determined** — no implementation-defined
   ordering.
3. **Consumable**: any adapter consumes the same events with the same
   code — no matter whether the source is a real `ObservableList`, a
   derived list, or a test fake.

---

## 1. ListChangeKind protocol

### D-1: six-value enum, never reordered

```cpp
enum class ListChangeKind {
    Insert,       // single-item insert
    Remove,       // single-item remove
    Replace,      // single-item replacement
    ItemChanged,  // T's own on_changed fired
    Reset,        // full clear
    Move,         // single-item position change
};
```

The numeric ordering is stable — **never reordered**, new entries can
only be appended. Reason: this enum crosses the ABI boundary
(`abi::SignalErased` multicasts `ListChange<T>`); reordering would
break binary compatibility.

### D-2: ListChange<T> field semantics

```cpp
struct ListChange<T> {
    ListChangeKind kind;
    std::size_t    index = 0;
    const T*       item  = nullptr;
    std::size_t    from_index = 0;   // only used by Move
};
```

| `kind` | meaning of `index` | meaning of `item` | `from_index` |
|---|---|---|---|
| `Insert` | element's position after insertion | raw pointer to the new element (lifetime ≥ emit) | unused, always 0 |
| `Remove` | element's position before removal | raw pointer to the removed element (**still valid during emit**) | unused, always 0 |
| `Replace` | position of the replaced element | raw pointer to the **new** element | unused |
| `ItemChanged` | element's current position | raw pointer to that element | unused |
| `Reset` | 0 | `nullptr` | unused |
| `Move` | position **after** the move | raw pointer to the moved element | position **before** the move |

### D-3: lifetime of the `item` pointer

The `item` pointer is **always valid for the duration of the emit
callback**:
- `Insert / Replace / ItemChanged / Move` — the element is in the
  list (or has just been added), so the underlying `shared_ptr<T>`
  refcount is ≥ 1.
- `Remove` — the framework **MUST** keep a temporary
  `shared_ptr<T>` keep-alive during the emit so observers can read
  `item` without it disappearing under them. `ObservableList` already
  does this (`std::vector<std::shared_ptr<T>> removed; ...
  emit(... removed.back().get() ...)`).
- `Reset` — `item == nullptr`; observers MUST NOT dereference.

Observers MUST NOT keep the `item` pointer past the emit callback. To
keep a long-lived reference, re-fetch `at(index)` from the source
list.

---

## 2. Event ordering protocol

### D-10: single-item ops emit exactly one event

| API | Event |
|---|---|
| `push_back(x)` / `emplace_back(...)` / `insert(pos, x)` | 1 × Insert |
| `remove_at(i)` / `remove_first(pred)` (when matched) | 1 × Remove |
| `replace_at(i, x)` | 1 × Replace |
| `move(from, to)` (from != to and both in-range) | 1 × Move |
| `clear()` | 1 × Reset |
| `T::on_changed` fires (only when T exposes it) | 1 × ItemChanged |

`move(from, to)` with `from == to` or out-of-range emits **no** event.

### D-11: batch ops emit multiple events, indices are "as observed"

`insert_range(pos, first, last)` and `remove_range(pos, count)` /
`remove_all(pred)` emit one single-element event per affected element:

- **Order**: events are emitted in operation order.
- **Index semantics**: each event's `index` reflects the list state
  **at the moment of THAT emit** — NOT the state at batch-start or
  batch-end.
  - `insert_range(0, [a, b, c])` → `Insert(0, a)`, `Insert(1, b)`,
    `Insert(2, c)`.
  - `remove_range(1, 2)` over `[A, B, C, D]` → `Remove(1, B)`,
    `Remove(1, C)` (the second emit sees the list as `[A, C, D]`).
- This guarantees that any observer rebuilding state incrementally
  from the event stream stays consistent.

### D-12: Reset means "discard everything"

After `clear()` emits `Reset`, the list is genuinely empty.
On receiving `Reset`, observers SHOULD:
1. wipe their mirror state;
2. NOT wait for follow-up Insert/Remove events (any subsequent
   `push_back` is a fresh stream).

### D-13: ItemChanged is best-effort, only when T fits the convention

`ObservableList<T>` installs a per-item subscription only when `T`
exposes `Subscription on_changed(std::function<void(const T&)>)`.
`Property<U>` and friends qualify; user-defined types that don't have
that signature simply never get `ItemChanged` events — by design, not
a bug.

ItemChanged on derived lists (FilteredList / SortedList / MappedList)
does NOT necessarily mirror upstream events 1:1:
- `FilteredList` only forwards ItemChanged for elements that pass its
  predicate.
- `MappedList` does NOT forward upstream ItemChanged (its output is a
  freshly-mapped T, with no `on_changed` concept).
- `SortedList` may emit a Move and an ItemChanged in either order
  when the change moves the element to a different sorted position
  (relative ordering between the two is implementation-private).

Derived-list specifics: see D-30.

### D-14: `reconcile` produces a normal event stream, never new semantics

`ObservableList<T>::reconcile(next, key_of)` brings the list in line with a
whole new sequence. It introduces **no new event kind and no new index rule**:
it drives the ordinary mutators, so every emission already obeys D-1, D-2 and
the D-11 "as observed" index rule.

Why it exists: every other mutator is imperative (the caller names the
operation), but server-backed data arrives declaratively — a whole new array,
with no indication of what moved. The alternatives were `clear()` +
`insert_range`, which emits Reset and therefore costs the observer its
selection, scroll position and row animations (D-12), or a hand-rolled diff in
user code, which forces the caller to track the intermediate coordinate system
`move(from, to)` operates in.

Guarantees:

1. **Identity** is decided by `key_of` (default: the element's address).
   Elements with equal keys are the same logical row across reconciles.
2. **Event mapping**:
   - key present before, absent after → `Remove`
   - key absent before, present after → `Insert`
   - key survives, handle differs → `Replace`
   - key survives, position differs → `Move` (never `Remove` + `Insert`,
     so DE5-conformant adapters keep their row animation)
   - key survives, handle and position identical → **no event**
3. **Atomicity of the batch**: the whole reconcile holds `emit_seq_`, so
   observers see one uninterrupted, correctly ordered run of events even if
   another thread is writing. Wrap the call in `reactive::batch` if
   downstream `Computed` values should recompute once at the end.
4. **No spurious Reset**: a reconcile emits `Reset` in exactly one case —
   `next` contains duplicate keys, which the keyed algorithm cannot
   represent. It then degrades to a clean rebuild rather than mis-diffing.
   An in-sync reconcile emits nothing and returns 0.
5. **Return value** is the number of events emitted.

Complexity is O(n) expected. This is a *sequence* reconcile, not a
minimum-edit-distance diff: it removes and inserts by key, then settles order
with at most one Move per out-of-place element. Myers would occasionally emit
one fewer Move, at the cost of O(ND) time and a substantially harder
correctness argument — and list adapters animate Move identically either way.

---

## 3. Re-entrancy semantics

### D-20: emit allows unsubscribe / subscribe / further mutation

Per [`lifecycle.md`](./lifecycle.md) **L-13** and **L-31**,
`abi::SignalErased::emit` is "snapshot-then-invoke":

- Disconnecting your own (or anyone else's) subscription from inside
  an event callback is safe; the current emit still runs the
  snapshot it took, the next emit honours the disconnect.
- Mutating the same list from inside an event callback (`push_back`,
  `remove_at`, etc.) is **allowed** but **not recommended** — it
  triggers nested emits and deepens the call stack. The framework
  does NOT auto-batch nested mutations; the caller decides whether
  to wrap in `reactive::batch`.

### D-21: exceptions thrown from a handler are swallowed

Exceptions escaping a `TypedSignal::connect`-installed handler are
caught by the invoker (consistent with [`lifecycle.md`](./lifecycle.md)
L-13 / **S-32**). Rationale: a single misbehaving handler must not
prevent subsequent handlers from running — observers must catch and
handle their own exceptions.

---

## 4. Derived-list contracts

Derived lists (FilteredList / SortedList / MappedList) still honour
**D-1 ... D-13** on their own emit stream, but their events do NOT
necessarily map 1:1 to upstream events.

### D-30: FilteredList

- Upstream `Insert(idx, x)`: emits `Insert(filtered_idx, x)` if
  `predicate(x)` is true; otherwise nothing.
- Upstream `Remove(idx, x)`: emits `Remove(filtered_idx)` if `x` was
  in the filtered view; otherwise nothing.
- Upstream `Replace(idx, new)`: emits 0/1/2 events based on the
  predicate result on `old` and `new` (precise mapping in
  `filtered_list.hpp`).
- Upstream `ItemChanged(idx, x)`: predicate result may have flipped
  → emits Insert / Remove / ItemChanged.
- Upstream `Reset`: emits `Reset`.
- Upstream `Move`: filtered order follows source order → may emit
  `Move`, may emit nothing (depends on whether the element is in the
  filtered view).

### D-31: SortedList

- Upstream `Insert(idx, x)`: locates the right position in the sorted
  view and emits `Insert(sorted_idx, x)`.
- Upstream `Remove`: emits `Remove(sorted_idx)`.
- Upstream `ItemChanged` that perturbs the order: may emit a `Move`
  + `ItemChanged` pair (relative ordering is observer-opaque).
- Upstream `Reset`: emits `Reset`.
- Upstream `Move`: **typically not observable** in the sorted view —
  sort already rearranged the elements, so a physical upstream move
  doesn't necessarily change the sorted order.

### D-32: MappedList<Source, Target>

- Strict 1:1 mapping: every upstream event maps to one local event
  with the same index (Map preserves position).
- ItemChanged is NOT forwarded (`Target` is derived through the mapper
  function and is not reactive itself).

---

## 5. ListSource concept

### D-40: ListSource is the adapter boundary

```cpp
template<typename L, typename T>
concept ListSourceOf = requires(L& l, std::function<void(const ListChange<T>&)> fn,
                                 std::size_t i) {
    { l.observe(std::move(fn)) } -> std::same_as<Subscription>;
    { l.size() }                 -> std::convertible_to<std::size_t>;
    { l.at(i) }                  -> std::convertible_to<std::shared_ptr<T>>;
    { l.snapshot() }             -> std::convertible_to<std::vector<std::shared_ptr<T>>>;
};
```

Any adapter that takes a list as `template<ListSource L>` automatically
honours D-1 ... D-32. Adapters MUST NOT specialise on the concrete
source type — every built-in derived list guarantees the same
contract.

---

## 6. Conformance suite — self-validation kit for any list impl

### D-50: `<aria/testing/list_conformance.hpp>`

Any **new derived-list implementation** (or test fake) MUST pass the
framework-provided conformance suite. The suite is templated on
`<ListSource L>` and translates every mechanically verifiable fact
from D-1 ... D-32 into doctest test cases.

- Adapter tests call `aria::testing::run_list_source_conformance<L>(factory)`
  to run every case automatically.
- `factory` is a `() -> shared_ptr<L>` callable that produces a fresh
  empty `L` on each call.
- The suite drives insert / remove / replace / move / reset / batch
  operations and asserts that the emitted event sequence matches the
  contract.

### D-51: implementations that pass today

- `ObservableList<T>` — backed directly by it; conformance acts as
  its source of truth.
- `FilteredList / SortedList / MappedList` — covered by dedicated
  tests for D-30 / D-31 / D-32.
- Any future derived list (incl. P1-roadmap items such as
  `GroupedList` / `WindowedList`) MUST run the conformance suite in
  the merging PR.

---

## 7. Anti-patterns

| # | Anti-pattern | Consequence | Correct approach |
|---|---|---|---|
| DE1 | Observer keeps `change.item` for the next frame | Element gets Removed → dangling pointer | Copy the element, or re-fetch `at(index)` from the list |
| DE2 | Observer assumes `list.size() == idx + 1` after `Insert(idx)` | Wrong — batch insert can make the list larger | Read `list.size()` if you need the size |
| DE3 | Observer reads `list.at(idx)` after `Remove(idx, ptr)` | Wrong — `idx` no longer points to `ptr`; may point to the next element or be out of range | The Remove `item` pointer is only valid during emit; discard after |
| DE4 | Throwing inside an emit callback | Exception is swallowed but can still corrupt state observed by later handlers | Use try/catch inside the handler |
| DE5 | Treating `Move(to=2, from=5)` as `Remove(5) + Insert(2)` | Adapters lose the "this is a move, not a destroy" signal | Adapters MUST distinguish Move from Remove+Insert |
| DE6 | Inside an `ItemChanged` callback, writing the element's `Property<T>` back | Feedback loop — relies on the equality gate to break or loops forever | `ItemChanged` is an observation event; do not `set` from inside it |

---

## 8. Cross-document references

- [`lifecycle.md`](./lifecycle.md) **L-13 / L-30 / L-31** specifies
  the snapshot semantics of emit, "install subscriptions outside the
  write lock", and observer ordering. D-20 / D-3 here depend
  directly on those.
- [`api-style.md`](./api-style.md) **S-3** requires that public types
  like `ListChangeKind` live under `aria::` — D-1 already complies.
- [`error-model.md`](./error-model.md) does not produce list errors
  (list mutations cannot fail), but a `Property<T>` element's
  `Validator` error stream may flow through `ItemChanged`, indirectly
  driving derived-list re-filtering.

---

## 9. Document governance

Every change to `ListChangeKind` / `ListChange` / `ListSource` MUST
flow as: doc change → code change → conformance-suite change.
Any new `ListChangeKind` value MUST first be registered in the D-1
table here together with an explicit ABI-compatibility strategy.
