# Migration guide

How to map concepts you already know — **Qt Signals/Slots**, **RxCpp**, and
**WPF / .NET MVVM** — onto Aria. This is a cheat-sheet, not a tutorial; see
`docs/guide/` for the full walkthroughs.

---

## From Qt Signals/Slots

| Qt | Aria | Notes |
|---|---|---|
| `QObject` property + `NOTIFY` signal | `Property<T>` | `set()` auto-notifies; equality-gated. |
| `connect(obj, &Cls::sig, ...)` | `prop.on_changed(fn)` / `prop.bind(fn)` | Returns a RAII `Subscription`; drop it to disconnect. |
| `Q_PROPERTY` computed getter | `Computed<T>([&]{ ... })` | Auto-tracks reads; no explicit dependency list. |
| `QObject::deleteLater` lifetime juggling | `Subscription` / `SubscriptionBag` | Weak-guarded disconnect; no use-after-free. |
| `QAbstractListModel` | `ObservableList<T>` + `qt_list_model_adapter` | Fine-grained Insert/Remove/Move/Replace diff stream. |
| `QAction::setEnabled` wiring | `Command<>` + `bind_command` | `enabled` auto-tracks the predicate. |
| Manual `QObject` thread affinity | reactive graph is single-thread; marshal via `runtime::Dispatcher` | See `docs/reference/lifecycle.md`. |

```cpp
// Qt:  emit countChanged(c);  connect(..., &View::setText)
// Aria:
Property<int> count{0};
Computed<std::string> label([&]{ return std::to_string(count.get()); });
auto sub = label.bind([](const std::string& s){ /* setText */ });
count = 1;   // label recomputes, bind fires
```

---

## From RxCpp / ReactiveX

| Rx | Aria | Notes |
|---|---|---|
| `BehaviorSubject<T>` | `Property<T>` | Holds a current value, multicasts changes. |
| `observable::combine_latest` | `combine_latest(a, b, fn)` (`property_ops.hpp`) | Or `Computed` for the glitch-free graph path. |
| `map` | `Computed` / `scan`'s projection | `Computed` is the idiomatic map. |
| `scan` / `reduce` | `scan(source, seed, reducer)` | In `property_ops.hpp`. |
| `distinctUntilChanged` | built-in (Property equality gate) or `distinct_until_changed(p)` | Property already drops equal-value writes. |
| `debounceTime` | `debounce(p, 300ms, scheduler)` | Needs an `IDelayedScheduler`. |
| `throttleFirst` | `throttle(p, cooldown, scheduler)` | Leading-edge. |
| `Subscription` / `CompositeDisposable` | `Subscription` / `SubscriptionBag` | Same RAII model. |
| `ObserveOn(scheduler)` | `EventBus::subscribe_on(dispatcher, ...)` / `co_await schedule_on(exec)` | |
| cold observable / `Task` | `aria::async::Task<T>` | Lazy, single-shot, exception-safe. |

```cpp
// Rx:  a.combineLatest(b, [](x,y){ return x+y; }).subscribe(...)
// Aria:
Property<int> a{1}, b{2};
auto sum = combine_latest(a, b, [](int x, int y){ return x + y; });
auto sub = sum->on_changed([](int v){ /* ... */ });
```

---

## From WPF / .NET MVVM

| WPF / .NET | Aria | Notes |
|---|---|---|
| `INotifyPropertyChanged` + backing field | `Property<T>` | No boilerplate `OnPropertyChanged`. |
| `ICommand` / `RelayCommand` | `Command<Args...>` | `CanExecute` predicate auto-tracks reactive reads. |
| `CommandManager.RequerySuggested` | automatic for `Command<>` | The built-in Effect re-evaluates `can_execute`. |
| `ObservableCollection<T>` | `ObservableList<T>` | + `CollectionView` ≈ `FilteredList`/`SortedList`/`MappedList`. |
| `CollectionView` filter/sort/group | `FilteredList` / `SortedList` / `GroupedList` (`aria/derived/`) | Reactive, incremental. |
| `IValueConverter` | `Converter<T, std::string>` + `bind_text_converted` | Two-way with `try_to_model`. |
| `{Binding Mode=TwoWay}` | `bind_text` / `bind_int` / ... | One-way variants are `bind_*_oneway`. |
| `SelectedItem` / `SelectedItems` | `Selection<T>` / `MultiSelection<T>` (`aria/selection.hpp`) | Reactive; `bind_to(list)` follows source mutations. |
| `Dispatcher.Invoke` | `runtime::IDispatcher::post` / `BindingEngine` `SmartMarshal` policy | |
| `async`/`await` + `CancellationToken` | `Task<T>` + `co_await` + `CancellationToken` | View-destroy cancel via `bind_view_lifetime`. |

```cpp
// WPF:  SelectedItem bound to ListBox; clears when item removed
// Aria:
Selection<Row> selection;
selection.bind_to(rows);                  // follows the ObservableList
auto sub = selection.selected().on_changed([](auto& row){ /* ... */ });
```

---

## Lifetime & threading (read before porting real code)

- The reactive graph (`Property`/`Computed`/`Effect`) is **single-threaded**.
  Update from a worker thread by marshalling through `runtime::Dispatcher`
  or `co_await schedule_on(ui)`. See `docs/reference/lifecycle.md`.
- `ObservableList`, `EventBus`, and `Command::can_execute` signals are
  thread-safe fire-and-forget (callbacks run outside the publisher lock).
- Every observer-adding API returns a `Subscription`. Store it (often in a
  `SubscriptionBag` on the owning ViewModel); dropping it disconnects.
