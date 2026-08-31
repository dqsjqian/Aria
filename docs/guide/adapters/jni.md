# JNI / Android Adapter

Aria supports two distinct Android integration shapes. Use the typed `JniAdapter` path for classic Android `View` objects; use a side-channel only when a UI host such as Jetpack Compose has no addressable view object for `BindingEngine` to bind.

## Typed View-backed path

`JniAdapter` implements the same typed `IViewAdapter` surface as the other first-party adapters. Its host-side contract test pins the class shape; the View-backed runtime lab in AriaTools is the behavioral gate. For Android `View`-backed screens, construct the adapter and wire properties and commands through `BindingEngine`; do not replace typed values with a string-keyed property protocol.

```cpp
// Called on the Android UI thread with real android.view.View objects.
auto adapter = std::make_shared<aria::adapters::jni::JniAdapter>(env);
aria::binding::BindingEngine engine(adapter);

aria::adapters::jni::JniView name_view(env, name_edit_text);
aria::adapters::jni::JniView submit_view(env, submit_button);
aria::adapters::jni::JniView status_view(env, status_text_view);

engine.bind_text(vm.name, name_view);                    // EditText ↔ Property<string>
engine.bind_command(vm.submit, submit_view);             // Button → Command
engine.bind_text_oneway(vm.status, status_view);         // Property/Computed → TextView

// Managed listeners call JNI entry points that forward through the same wrappers:
adapter->notify_text_changed(name_view, new_text);
adapter->notify_click(submit_view);
```

`on_*_changed` installs the C++ subscription; the Java/Kotlin listener forwards the native event through the matching `notify_*` method. Listener ownership remains on Android while `BindingEngine` stays typed.

`JniView` owns a JNI global reference, so its C++ wrapper must follow the native screen's lifetime. The end-to-end View-backed lab belongs to [AriaTools](https://github.com/dqsjqian/AriaTools), Aria's flagship cross-platform application for Qt, iOS, Android, and Web.

## Lists: RecyclerView

`JniListSource<T>` is the Android counterpart of Qt6's `ObservableListModel`, UIKit's `ObservableTableSource` and the AppKit table source. It accepts any source satisfying `aria::ListSourceOf<L, T>` — `ObservableList`, `FilteredList`, `SortedList`, `MappedList`, `DistinctList`, `PagedList`, `GroupedList` — and turns `ListChange<T>` events into RecyclerView notifications:

| `ListChangeKind` | RecyclerView call |
|---|---|
| `Insert` | `notifyItemInserted(position)` |
| `Remove` | `notifyItemRemoved(position)` |
| `Replace` | `notifyItemChanged(position)` |
| `ItemChanged` | `notifyItemChanged(position)` |
| `Move` | `notifyItemMoved(from, to)` |
| `Reset` | `notifyDataSetChanged()` |

Rows stay `std::shared_ptr<T>`, so item identity — and therefore selection and per-row diffing — survives the hop. Do **not** join items into one string and split them in Kotlin: that discards exactly what this bridge exists to preserve.

The bridge is split in two, and the split is deliberate:

- **`JniListSource<T>`** (`JniListSource.hpp`) owns the snapshot and the diffing, and has **no `<jni.h>` dependency**. Its row arithmetic therefore runs in the ordinary host test suite (`jni_list_source` in `ctest`), unlike the rest of this adapter, which can only be static-asserted without a live VM.
- **`JniRecyclerNotifier`** (`JniRecyclerNotifier.hpp`) is the JNI half: it holds a global ref to the managed `RecyclerView.Adapter`, resolves the `notifyItem*` method IDs once, and exposes `sink()`.

```cpp
// On the Android UI thread. `adapter` is the Kotlin RecyclerView.Adapter.
auto notifier = std::make_shared<aria::adapters::jni::JniRecyclerNotifier>(
    env, adapter);
// Check during bring-up: a failed method lookup silently drops updates.
if (!notifier->valid()) { /* report and bail */ }

auto rows = std::make_unique<aria::adapters::jni::JniListSource<Movie>>(
    vm.movies, notifier->sink());
```

Then forward the managed adapter's overrides back through JNI:

| Kotlin override | C++ call |
|---|---|
| `getItemCount()` | `rows->item_count()` |
| `onBindViewHolder(holder, position)` | `rows->at(position)` |

`at()` returns `nullptr` for an out-of-range position rather than throwing, because the managed side may ask about a row a pending notification has already removed. `reload()` resyncs from the source and raises a single `notifyDataSetChanged()` — the escape hatch for an adapter re-attached after a configuration change.

Two contracts worth stating explicitly:

- **Threading.** RecyclerView mutation must happen on the main looper. `JniListSource` does **not** hop threads — Aria owns no looper abstraction, and adding one would exceed an adapter's remit. The sink runs on whichever thread emitted the change, so if your producer can emit off-main, wrap `sink()` in a lambda that posts to a `Handler`. The `shared_ptr<T>` is resolved at emit time, so a deferred sink still sees the row that was live when the change was announced.
- **`notifyItemMoved` takes the raw `(from, to)` pair.** Unlike `QAbstractItemModel::beginMoveRows`, it needs no `+1` adjustment for downward moves. This is pinned by a test, because it is the classic porting bug.

`JniRecyclerNotifier` must outlive the `JniListSource` that holds its sink.

## Compose side-channel path

Compose state does not expose addressable Android `View` instances, so the typed view adapter is not the right bridge for composables. A side-channel can instead:

1. keep reactive state in the C++ ViewModel;
2. subscribe to the relevant properties;
3. forward updates through JNI into typed Kotlin `StateFlow` holders;
4. let Compose collect those flows and recompose.

Scope this bridge to the Compose boundary. It is not a general replacement for `JniAdapter`, and application code should preserve native types rather than funneling unrelated properties through a single string map.

## Building the framework for Android

Prerequisites:

- Android NDK 29+ (Clang 20)
- CMake 3.22+ (bundled with Android SDK)

```bash
./scripts/build.sh android
```

For a runnable Android application and the View-backed typed adapter lab, follow the build instructions in [AriaTools](https://github.com/dqsjqian/AriaTools).
