# Collections

Aria provides `ObservableList<T>` as the mutable source and a family of derived list views that transform, filter, sort, and paginate — all reactive, all change-propagating.

**Include:** `#include "aria/observable_list.hpp"` and derived views from `#include "aria/derived/filtered_list.hpp"`, etc.

---

## ObservableList\<T\>

Thread-safe observable collection of `std::shared_ptr<T>`. Fires granular `ListChange<T>` events on every mutation.

### Create and Populate

```cpp
aria::ObservableList<Task> tasks;

auto t1 = std::make_shared<Task>("Write docs");
tasks.append(t1);

auto t2 = std::make_shared<Task>("Ship release");
tasks.append(t2);
```

### Mutations

```cpp
// Insert at position
tasks.insert(0, std::make_shared<Task>("Urgent"));

// Remove by index
tasks.remove_at(1);

// Replace item at index
tasks.replace(0, std::make_shared<Task>("Updated"));

// Move item from index A to index B
tasks.move(2, 0);

// Clear all
tasks.clear();
```

### Range Operations

```cpp
std::vector<std::shared_ptr<Task>> batch = {/* ... */};
tasks.insert_range(tasks.size(), batch.begin(), batch.end());

tasks.remove_range(0, 3);  // remove 3 items starting at index 0

tasks.remove_all([](const std::shared_ptr<Task>& t) {
    return t->done;
});
```

### Syncing With a Fresh Snapshot (`reconcile`)

The mutators above are imperative — you name the operation. But when data
arrives from a server you usually get a whole new array with no indication of
what changed. `reconcile` works out the difference for you:

```cpp
// Rows are the same logical row iff their id matches, even though the
// server hands us freshly allocated objects every refresh.
struct ById {
    int operator()(const Task& t) const noexcept { return t.id; }
};

std::vector<std::shared_ptr<Task>> fresh = fetch_tasks();
tasks.reconcile(std::move(fresh), ById{});
```

This emits the minimal edit stream — `Insert` / `Remove` / `Replace` /
`Move` — rather than a `Reset`. That distinction matters: on `Reset`
observers must discard their mirror, so the view loses selection, scroll
position, expansion state and row animations. A poll loop built on
`clear()` + `insert_range` throws all of that away on every tick, even when
nothing actually changed. `Selection` also clears itself on `Reset`, so a
refresh would silently drop whatever the user had selected.

Pass a real `key_of` whenever the source allocates new objects for the same
logical rows; the default identity is the object's address, which is only
useful if you reuse handles. Reconciling an already-matching list emits
nothing and returns 0.

Wrap the call in `reactive::batch` if downstream `Computed` values should
recompute once at the end rather than per event.

Full semantics: `docs/reference/list-diff-contract.md` D-14.

### Observe Changes

```cpp
auto sub = tasks.on_change([](const aria::ListChange<Task>& change) {
    switch (change.kind) {
        case aria::ListChangeKind::Insert:
            std::cout << "Inserted at " << change.index << "\n";
            break;
        case aria::ListChangeKind::Remove:
            std::cout << "Removed at " << change.index << "\n";
            break;
        case aria::ListChangeKind::Replace:
            std::cout << "Replaced at " << change.index << "\n";
            break;
        case aria::ListChangeKind::Move:
            std::cout << "Moved from " << change.from_index
                      << " to " << change.index << "\n";
            break;
        case aria::ListChangeKind::Reset:
            std::cout << "List cleared\n";
            break;
        case aria::ListChangeKind::ItemChanged:
            std::cout << "Item changed at " << change.index << "\n";
            break;
    }
});
```

### Read

```cpp
std::size_t n = tasks.size();
bool empty = tasks.empty();

auto item = tasks.at(3);  // shared_ptr<Task>, or nullptr if out of range
tasks.index_of(item);     // O(1) amortised via internal hash map
```

### ListChangeKind Reference

| Kind | Index | Item | From Index | Meaning |
|------|-------|------|------------|---------|
| `Insert` | insertion point | inserted item | — | One item added |
| `Remove` | slot before removal | removed item | — | One item removed |
| `Replace` | position | new item | — | Item swapped |
| `Move` | destination | moved item | source | Item relocated |
| `Reset` | 0 | nullptr | — | Entire list cleared |
| `ItemChanged` | current position | changed item | — | Item's own `on_changed` fired |

---

## Derived Views

Derived views sit on top of an `ObservableList` (or another derived view) and transform the stream. They are themselves `ListSource` implementations — you can chain them.

### FilteredList

Show only items matching a predicate:

```cpp
#include "aria/derived/filtered_list.hpp"

aria::ObservableList<Task> all_tasks;

aria::FilteredList<Task> incomplete{all_tasks,
    [](const std::shared_ptr<Task>& t) { return !t->done; }
};
```

When `all_tasks` mutates, `incomplete` recalculates and emits its own `ListChange` events.

### SortedList

Maintain items in sorted order:

```cpp
#include "aria/derived/sorted_list.hpp"

aria::SortedList<Task> sorted{all_tasks,
    [](const std::shared_ptr<Task>& a, const std::shared_ptr<Task>& b) {
        return a->priority > b->priority;  // highest first
    }
};
```

### MappedList

Transform each item into a different type:

```cpp
#include "aria/derived/mapped_list.hpp"

aria::MappedList<std::string> names{all_tasks,
    [](const std::shared_ptr<Task>& t) -> std::shared_ptr<std::string> {
        return std::make_shared<std::string>(t->title);
    }
};
```

### DistinctList

Remove duplicates by key:

```cpp
#include "aria/derived/distinct_list.hpp"

aria::DistinctList<Task> unique{all_tasks,
    [](const std::shared_ptr<Task>& t) { return t->id; }
};
```

### PagedList

Virtualize a large list into pages:

```cpp
#include "aria/derived/paged_list.hpp"

aria::PagedList<Task> paged{all_tasks, /*page_size=*/20};
paged.set_page(0);  // show first 20 items
paged.page_count();  // total pages
```

### GroupedList

Group items by a key function:

```cpp
#include "aria/derived/grouped_list.hpp"

aria::GroupedList<Task> grouped{all_tasks,
    [](const std::shared_ptr<Task>& t) { return t->category; }
};
```

---

## Chaining

Derived views compose — filter first, then sort:

```cpp
aria::ObservableList<Task> source;

aria::FilteredList<Task> active{source,
    [](const auto& t) { return !t->done; }};

aria::SortedList<Task> sorted{active,
    [](const auto& a, const auto& b) { return a->priority > b->priority; }};
```

Mutations to `source` propagate through `active` → `sorted` automatically.

---

## Quick Reference

| Type | Input | Transform | Output Events |
|------|-------|-----------|---------------|
| `ObservableList<T>` | Direct mutations | — | Insert/Remove/Replace/Move/Reset/ItemChanged |
| `FilteredList<T>` | Any `ListSource<T>` | Predicate filter | Same kinds, subset |
| `SortedList<T>` | Any `ListSource<T>` | Comparator sort | Same kinds, reordered |
| `MappedList<U>` | Any `ListSource<T>` | T → U transform | Same kinds, mapped items |
| `DistinctList<T>` | Any `ListSource<T>` | Deduplicate by key | Same kinds, deduplicated |
| `PagedList<T>` | Any `ListSource<T>` | Window by page | Same kinds, windowed |
| `GroupedList<T>` | Any `ListSource<T>` | Group by key | Group-aware events |

---

## See Also

- [Reactive Core →](reactive-core.md) — `Property` and `Computed` that power list observations
- [List Diff Contract →](../reference/list-diff-contract.md) — authoritative event-sequence specification
- [Adapters →](adapters/) — platform-specific list binding (Qt `QAbstractListModel`, AppKit `NSTableView`, etc.)
