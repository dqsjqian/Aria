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
    Reset,        // replace the whole mirror with snapshot
    Move,         // single-item position change
};
```

The numeric ordering is stable — **never reordered**, new entries can
only be appended. Reason: this enum crosses the ABI boundary
(`abi::SignalErased` multicasts `ListChange<T>`); reordering would
break binary compatibility.

### D-2: ListChange<T> field semantics

```cpp
template<class T>
struct ListChange {
    ListChangeKind kind;
    std::size_t index = 0;
    std::shared_ptr<T> item;
    std::size_t from_index = 0;
    std::shared_ptr<const std::vector<std::shared_ptr<T>>> snapshot;
};
```

| `kind` | meaning of `index` | meaning of `item` | `from_index` |
|---|---|---|---|
| `Insert` | position after insertion into the receiver's mirror | owning handle to the inserted element | 0 |
| `Remove` | position before removal from the mirror | owning handle to the removed element | 0 |
| `Replace` | position of the replaced element | owning handle to the new element | 0 |
| `ItemChanged` | element's position in the mirror | owning handle to that element | 0 |
| `Reset` | 0 | empty | 0 |
| `Move` | position after the move in the mirror | owning handle to the moved element | position before the move |

`Reset` always carries a non-null `snapshot`, including for an empty list.
Other kinds carry no snapshot. The snapshot contains the complete replacement
sequence, not another stream of edits.

### D-3: event payloads own their elements

Copying an event retains its `item` or `snapshot`, so adapters can queue it
across threads without borrowing an element from the source. The pointed-to
`T` remains a shared, potentially mutable object: owning an event preserves
identity and lifetime, not a deep historical copy of each field.

Consumers MUST use `change.item` and `change.snapshot` to interpret events.
They MUST NOT fetch `source.at(change.index)` or a later source snapshot to
recover an event payload. The source may already contain later batch edits
or a reentrant mutation made by an earlier observer.

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

### D-11: batch events use incremental mirror coordinates

Batch operations commit their structural changes efficiently, then deliver
an ordered edit stream. Apply each event to the result of applying all
preceding events; the producer may already hold the final batch state.

- `insert_range(0, [a, b, c])` emits `Insert(0, a)`, `Insert(1, b)`,
  `Insert(2, c)`.
- `remove_range(1, 2)` over `[A, B, C, D]` emits `Remove(1, B)` and
  `Remove(1, C)`. The receiver's mirror becomes `[A, C, D]`, then `[A, D]`.
- Every batch is queued before fanout. Reentrant edits are appended after
  the already committed batch and delivered after the current fanout.

This permits one vector insertion or compaction for a whole range while
keeping a deterministic stream for every adapter. Source reads during a
callback are live reads; event coordinates belong to the receiver's mirror.

### D-12: Reset replaces the complete mirror

On `Reset`, replace the mirror with `*change.snapshot`. The replacement may
be nonempty, for example after a derived view rebuild. No follow-up inserts
are required to describe that snapshot. `clear()` emits an empty snapshot.

### D-13: ItemChanged is best-effort, only when T fits the convention

`ObservableList<T>` installs a per-item subscription only when `T`
exposes `Subscription on_changed(std::function<void(const T&)>)`.
One subscription is installed per distinct object handle. If the same object
occupies multiple rows, ItemChanged emits once for every valid occurrence,
using indices frozen when the notification was produced.
`Property<U>` and friends qualify; user-defined types that don't have
that signature simply never get `ItemChanged` events — by design, not
a bug.

ItemChanged on derived lists (FilteredList / SortedList / MappedList)
does NOT necessarily mirror upstream events 1:1:
- `FilteredList` only forwards ItemChanged for elements that pass its
  predicate.
- `MappedList` remaps the element and emits Replace with the new owning
  handle, so downstream mirrors adopt the new object identity.
- `SortedList` emits Move followed by ItemChanged at the destination when
  the value changes its sorted position. Both belong to one ordered batch.
  A value change that retains its position emits only ItemChanged.

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

An unchanged sequence or append-only update takes expected O(n) work.
Arbitrary reorderings can take O(n²) because suffix lookup and vector moves
are linear. This is a keyed sequence reconciliation, not a minimum-edit-distance
algorithm. Null target handles are ignored. Duplicate target keys produce one
empty Reset followed by inserts, all in the same ordered batch.

---

## 3. Re-entrancy semantics

### D-20: unsubscribe, subscribe, and reentrant edits

Each fanout snapshots its subscribers and checks connection activity before
invocation. Disconnecting a later subscriber prevents its callback in the
current fanout; a callback already executing on another thread may finish.
No user callback or capture destructor runs under the signal registry lock.

List signals serialize nested mutations behind their current fanout and any
already queued batch. They do not recursively deliver the nested edit ahead
of older events. Reactive batching is separate and controls graph flushes.

New subscribers skip events committed before their subscription, including
the remaining events of an already queued batch. This allows a view created
inside an observer to initialize from the current source snapshot without
replaying that old batch twice. Initialization through separate `snapshot()`
and `observe()` calls must still be serialized against concurrent writers;
those two calls are not an atomic operation.

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
- Upstream `ItemChanged` that changes the order emits Move followed by
  ItemChanged at the destination. Downstream predicates and mappers therefore
  see both the new position and the changed value.
- Upstream `Reset`: emits `Reset`.
- Upstream `Move`: **typically not observable** in the sorted view —
  sort already rearranged the elements, so a physical upstream move
  doesn't necessarily change the sorted order.

### D-32: MappedList<Source, Target>

- Mapping preserves positions. Insert, Remove, Replace, and Move retain
  their corresponding mirror coordinates.
- ItemChanged remaps the source element and emits Replace with the new
  target handle. Reset carries the complete mapped snapshot.

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

### D-41: derived lists compose

Every derived list takes its source as a template parameter constrained to
`ListSourceOf<Source, T>`, defaulting to `ObservableList<T>`:

```cpp
template<typename T, typename Source = ObservableList<T>>
    requires ListSourceOf<Source, T>
class FilteredList { ... };
```

That means a derived list is itself a legal source for another one, so
pipelines are expressible:

```cpp
auto evens  = aria::filtered(src,   [](const Row& r) { return r.v % 2 == 0; });
auto unique = aria::distinct<int>(evens, [](const Row& r) { return r.v; });
auto asc    = aria::sorted(unique,  [](auto& a, auto& b) { return a.v < b.v; });
auto page= aria::paged(asc, /*page_size=*/20);
```

Guarantees:

1. **Propagation is transitive** — a mutation on the root list reaches the
   far end of the chain, with each stage applying its own transformation and
   emitting per D-1/D-2/D-11 as usual.
2. **Lifetime is transitive** — each stage holds a strong `shared_ptr` to its
   source, so holding the last stage keeps the whole chain alive. Intermediate
   handles may be dropped.
3. **No behavioural change for single-level use.** The default source type
   means existing spellings (`FilteredList<Row> f{src, pred};`) are unaffected.

Prefer the `filtered()` / `sorted()` / `paged()` / `mapped<Target>()` /
`distinct<Key>()` / `grouped<Key>()` factory helpers: they deduce the source
type, so the chain does not have to be spelled out
(`SortedList<Row, FilteredList<Row>>` and so on).

Ordering caveat: composing stages that each re-order (for example
`sorted -> sorted`) is legal but pointless — the last stage wins. Compose
stages that do different jobs.

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
| DE1 | Observer keeps only `change.item.get()` for a later frame | Dropping the owning event may destroy the element | Retain the shared handle or event |
| DE2 | Observer interprets an event using live source size | The source may already contain later edits | Apply edits to an incremental mirror |
| DE3 | Observer recovers any event payload using `list.at(idx)` | The indexed row may already have changed | Use the owning `item` or Reset `snapshot` |
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
  (allocation, predicates, and other user callbacks can fail), but a `Property<T>` element's
  `Validator` error stream may flow through `ItemChanged`, indirectly
  driving derived-list re-filtering.

---

## 9. Document governance

Every change to `ListChangeKind` / `ListChange` / `ListSource` MUST
flow as: doc change → code change → conformance-suite change.
Any new `ListChangeKind` value MUST first be registered in the D-1
table here together with an explicit ABI-compatibility strategy.
