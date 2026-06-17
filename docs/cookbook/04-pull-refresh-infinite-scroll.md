# Recipe 4 — Pull-to-refresh + infinite scroll

**Goal:** a feed that (a) re-fetches from the top on pull-to-refresh, and
(b) grows its visible window as the user scrolls (infinite scroll).

Two orthogonal pieces:

- **`AsyncResource<T, Key>`** owns the *fetch* lifecycle — loading flag,
  error, stale-while-revalidate cache, dedupe (see
  `docs/reference/error-model.md`, the `R-1` invariant, and
  `aria/async/async_resource.hpp`).
- **`PagedList<T>`** owns the *windowing* over an already-loaded
  `ObservableList<T>` — page index / page size are live Properties
  (PG-2; see `aria/derived/paged_list.hpp`).

## Pull-to-refresh

```cpp
#include "aria/async/async_resource.hpp"

aria::async::AsyncResource<Feed, int> feed{ui_executor, net_pool,
    [](int page) -> aria::async::Task<Feed> { co_return co_await api::feed(page); }};

// Spinner + content bind to the resource's observable surface.
auto load_sub = feed.is_loading.on_changed([](bool b){ spinner.visible = b; });
auto data_sub = feed.data.on_changed([](const std::optional<Feed>& f){
    if (f) render(*f);
});

void on_pull_to_refresh() {
    feed.refresh();   // force a refetch of the current key; keeps old data visible (SWR)
}
```

On refresh, `is_loading` flips true while the previous `data` stays
visible (the `Refreshing` state of the synthesised `Loadable<Feed>`
surface — bind `feed.loadable` for a single sum-type instead of four
flags).

## Infinite scroll

Back the visible rows with a `PagedList` and grow the window when the
user nears the bottom:

```cpp
#include "aria/derived/paged_list.hpp"

auto items = std::make_shared<aria::ObservableList<Item>>();
aria::PagedList<Item> window{items, /*initial_page_size=*/20};

// The table binds to the PagedList's change stream like any list.
auto win_sub = window.observe([&](const aria::ListChange<Item>& ch){ table.apply(ch); });

void on_scrolled_near_bottom() {
    if (!window.is_last_page()) {
        // "Infinite scroll" = grow the page size so more rows enter the
        // window. (Alternatively, advance window.page_index() for
        // discrete paging.)
        window.page_size().set(window.page_size().get() + 20);
    }
}
```

`page_size()` / `page_index()` are `Property<std::size_t>&` — setting
either re-windows and emits the minimal Insert/Remove diff, so the table
animates in only the newly-visible rows rather than reloading.

## Combining the two

The usual shape: an `AsyncResource` (or a paginated fetch loop) appends
fetched items into the `ObservableList<Item>` source; the `PagedList`
windows it for the table; pull-to-refresh clears the source and
re-fetches page 0. Each concern stays independently testable.
