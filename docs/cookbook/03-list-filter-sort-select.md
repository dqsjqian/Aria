# Recipe 3 — List view: filter + sort + selection

**Goal:** show a live list that the user can filter and sort, with a
selection model that stays consistent as the underlying data changes.

Uses `ObservableList<T>` (source of truth), the derived-list family
(`FilteredList<T>` / `SortedList<T>`), and `Selection<T>` /
`MultiSelection<T>` (SE-1..SE-5; see `aria/selection.hpp`).

```cpp
#include "aria/observable_list.hpp"
#include "aria/derived/filtered_list.hpp"
#include "aria/derived/sorted_list.hpp"
#include "aria/selection.hpp"

struct Contact { std::string name; bool favourite; };

auto source = std::make_shared<aria::ObservableList<Contact>>();

// Live filtered view: favourites only. The predicate is a heap-free
// inplace_function (≤32-byte capture; LD/L-31.5).
aria::FilteredList<Contact> favourites{
    source, [](const Contact& c){ return c.favourite; }};

// Live sorted view over the SAME source, alphabetical by name.
aria::SortedList<Contact> by_name{
    source, [](const Contact& a, const Contact& b){ return a.name < b.name; }};

// Single selection, bound to the source list.
aria::Selection<Contact> selected;
selected.bind_to(*source);   // removed/replaced item auto-clears selection
```

## Driving the views from a table adapter

Each derived view exposes the same read + observe surface as
`ObservableList` (`size()` / `at()` / `snapshot()` / `observe()` /
`on_any_change()`), so a table adapter binds to a derived view exactly as
it would to the source:

```cpp
auto sub = favourites.observe([&](const aria::ListChange<Contact>& ch){
    table.apply(ch);   // Insert/Remove/Move/Replace/Reset/ItemChanged
});
```

## Selection semantics (SE-3)

`bind_to` keeps the selection coherent under source mutations:

- an item **removed** from the source (Remove / Reset) drops out of the
  selection;
- a **Replace** at the selected slot drops it (the element changed
  identity);
- **Move / Insert** of other elements leaves the selection intact.

```cpp
selected.select(source->at(0));
auto sel_sub = selected.selected().on_changed(
    [](const std::shared_ptr<Contact>& c){ detail_pane.show(c); });

source->remove_at(0);                  // selection auto-clears
assert(!selected.has_value());
```

## Visible vs underlying selection (SE-4, a documented idiom)

There is intentionally **no** "bind selection to the filtered view"
method. Pick the list whose identity you want to track:

- **Track the underlying data** → `selected.bind_to(*source)`. A
  selected item that is filtered *out of view* stays selected (it just
  isn't currently visible). This is usually what you want for a
  master/detail pane. `bind_to` only accepts an `ObservableList<T>&`,
  which is exactly this case.
- **Track the visible rows** → a `FilteredList` is *not* an
  `ObservableList`, so you wire it by hand: observe the derived view's
  change stream and clear the selection when the selected item leaves
  the view. This is the SE-4 idiom (a recipe, not a method):

  ```cpp
  auto vis_sub = favourites.observe(
      [&](const aria::ListChange<Contact>& ch){
          using K = aria::ListChangeKind;
          if (ch.kind == K::Reset) { selected.clear(); return; }
          if ((ch.kind == K::Remove || ch.kind == K::Replace) &&
              selected.value().get() == ch.item) {
              selected.clear();   // selected row scrolled out of the filter
          }
      });
  ```
  Now filtering an item out clears the selection, matching a "selection
  follows what's on screen" UX.

For multi-select (e.g. checkboxes), swap `Selection<T>` for
`MultiSelection<T>` — `add` / `remove` / `toggle`, with `values()`
returning the picks in pick-order.
