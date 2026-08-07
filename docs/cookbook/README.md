# Aria Cookbook

Short, self-contained recipes for common MVVM tasks. Each recipe is a
*how-to* grounded in shipped APIs and cross-links the authoritative
contract docs (`docs/reference/*.md`) and module guides
(`docs/guide/*.md`). For the full symbol-level reference, build the
Doxygen API docs:

```bash
cmake -B build/flavors/docs -DARIA_BUILD_DOCS=ON
cmake --build build/flavors/docs --target aria_docs
open build/docs/html/index.html
```

## Recipes

1. [Form with sync + async rules](01-form-sync-async-rules.md)
2. [Cross-field rule (password == confirm, start ≤ end)](02-cross-field-rule.md)
3. [List view: filter + sort + selection](03-list-filter-sort-select.md)
4. [Pull-to-refresh + infinite scroll](04-pull-refresh-infinite-scroll.md)
5. [Theme / Locale reactive switching](05-theme-locale-switching.md)
6. [Async `with_timeout` + `when_any` race](06-timeout-when-any-race.md)
7. [View-destroy cancellation](07-view-destroy-cancellation.md)
8. [Writing a new `IViewAdapter`](08-writing-a-view-adapter.md)

## Conventions

- Code uses the public `aria::` surface (never an implementation
  namespace — see `docs/reference/api-style.md`, S-1).
- All graph writes (`Property::set`, `Computed`, `Effect`,
  `ObservableList` mutations) happen on the graph thread; see
  `docs/reference/lifecycle.md` L-1.
- Each recipe pins the relevant contract IDs (`L-N`, `E-N`, `V-N`,
  `SE-N`, `LD-N`, `PG-N`) so you can trace the guarantee to its source.
