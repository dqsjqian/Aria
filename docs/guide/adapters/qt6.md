# Qt6 Adapter

Integrating Aria with Qt 6.x desktop applications. The Qt6 adapter provides `QtAdapter` (widget binding), `QtView` (view wrapper), `ObservableListModel` (list binding), `bind_combo_box_selection` (stable-ID selection), and `QtDispatcher` (event loop bridge).

**Include:** `#include "aria/adapters/qt6/qt_adapter.hpp"`, etc.

---

## Setup

### Include and Link

```cmake
find_package(aria REQUIRED COMPONENTS qt6)
target_link_libraries(myapp PRIVATE aria::qt6)
```

### Initialize

```cpp
#include "aria/adapters/qt6/qt_adapter.hpp"
#include "aria/adapters/qt6/qt_dispatcher.hpp"
#include "aria/binding/binding_engine.hpp"

// In main():
QApplication app(argc, argv);

auto dispatcher = std::make_shared<aria::adapters::qt6::QtDispatcher>(&app);
aria::runtime::set_main_dispatcher(dispatcher);

auto adapter = std::make_shared<aria::adapters::qt6::QtAdapter>();
aria::binding::BindingEngine engine(adapter, dispatcher,
    aria::binding::BindingEngine::DispatchPolicy::SmartMarshal);
```

---

## QtView — Wrap Any Widget

```cpp
#include "aria/adapters/qt6/qt_view.hpp"

// Wrap a native QWidget*
auto view = std::make_shared<aria::adapters::qt6::QtView>(lineEdit);
```

`QtView` uses `QPointer` internally — if the widget is destroyed, the pointer nulls out safely. No dangling references.

### Type-Safe Cast

```cpp
auto* line_edit = view->as<QLineEdit>();
```

---

## Widget Bindings

### Text (QLabel / QLineEdit / QTextEdit / QComboBox)

```cpp
aria::Property<std::string> name{"Alice"};

// Two-way: user typing → Property, Property → widget text
auto line_edit_view = std::make_shared<aria::adapters::qt6::QtView>(lineEdit);
engine.bind_text(name, *line_edit_view);

// One-way: Property → QLabel display only
auto label_view = std::make_shared<aria::adapters::qt6::QtView>(label);
engine.bind_text_oneway(name, *label_view);
```

### Bool (QCheckBox / QRadioButton)

```cpp
aria::Property<bool> dark_mode{false};

auto checkbox_view = std::make_shared<aria::adapters::qt6::QtView>(checkbox);
engine.bind_bool(dark_mode, *checkbox_view);
```

### Int (QSpinBox / QSlider / QDial)

```cpp
aria::Property<int> volume{50};

auto slider_view = std::make_shared<aria::adapters::qt6::QtView>(slider);
engine.bind_int(volume, *slider_view);
```

### Double (QDoubleSpinBox)

```cpp
aria::Property<double> price{9.99};

auto spinbox_view = std::make_shared<aria::adapters::qt6::QtView>(doubleSpinbox);
engine.bind_double(price, *spinbox_view);
```

### Visible / Enabled (Any QWidget)

```cpp
aria::Property<bool> has_results{false};

auto container_view = std::make_shared<aria::adapters::qt6::QtView>(resultsPanel);
engine.bind_visible(has_results, *container_view);
engine.bind_enabled(can_save, *save_button_view);
```

### Click (QPushButton / QToolButton)

```cpp
aria::Command<> save_cmd{[&] { do_save(); }};

auto btn_view = std::make_shared<aria::adapters::qt6::QtView>(saveButton);
engine.bind_command(save_cmd, *btn_view);
// Button click → save_cmd.execute()
// save_cmd.can_execute() drives button->setEnabled()
```

---

## Supported Widgets Summary

| Widget | Binding | Direction |
|--------|---------|-----------|
| `QLabel` | `bind_text_oneway` | VM→View |
| `QLineEdit` | `bind_text` | Two-way |
| `QPlainTextEdit` / `QTextEdit` | `bind_text` | Two-way |
| `QComboBox` displayed text | `bind_text` | Two-way |
| `QComboBox` fixed option index | `bind_int` / `bind_int_converted` | Two-way |
| `QComboBox` stable item ID | `bind_combo_box_selection` | VM→View / user activation→VM |
| `QCheckBox` / `QRadioButton` | `bind_bool` | Two-way |
| `QSpinBox` / `QSlider` / `QDial` | `bind_int` | Two-way |
| `QDoubleSpinBox` | `bind_double` | Two-way |
| `QPushButton` / `QToolButton` | `bind_command` | View→VM |
| Any `QWidget` | `bind_visible` / `bind_enabled` | VM→View |

---

## ObservableListModel — Drive QListView / QTableView

Bridge any `ObservableList<T>` (or derived list) onto `QAbstractListModel`:

```cpp
#include "aria/adapters/qt6/qt_list_model_adapter.hpp"

// Define roles
QHash<int, QByteArray> roles;
roles[Qt::DisplayRole] = "display";
roles[256] = "priority";

// Define role accessor
auto role_fn = [](const Task& t, int role) -> QVariant {
    switch (role) {
        case Qt::DisplayRole: return QString::fromStdString(t.title);
        case 256: return t.priority;
        default: return {};
    }
};

// Create model
aria::adapters::qt6::ObservableListModel<Task> model{
    vm.tasks, roles, role_fn};

// Assign to QListView
listView->setModel(&model);
```

### With Derived Lists

```cpp
auto active = std::make_shared<aria::FilteredList<Task>>(
    vm.tasks_shared(), [](const Task& t) { return !t.done; });

aria::adapters::qt6::ObservableListModel<Task> model{
    *active, roles, role_fn};
```

Changes to the source list propagate through the derived list → model → view automatically.

### Event Mapping

| Aria Event | Qt Model Signal |
|------------|-----------------|
| `Insert` | `beginInsertRows` / `endInsertRows` |
| `Remove` | `beginRemoveRows` / `endRemoveRows` |
| `Replace` / `ItemChanged` | `dataChanged` |
| `Move` | `beginMoveRows` / `endMoveRows` |
| `Reset` | `beginResetModel` / `endResetModel` |

### Thread Safety

If the source list mutates from a background thread, the model automatically queues the change to the Qt event loop via `QMetaObject::invokeMethod`. Each queued change retains its item or Reset snapshot and is replayed in order; the model does not reread a source index that a later mutation may have changed.

---

## QComboBox — Options and Selection

Use text binding when the displayed text is the value. For fixed option
positions, `bind_int` maps to `currentIndex()` (`-1` means no selection).
`bind_int_converted` keeps the ViewModel enum-typed and validates indices:

```cpp
enum class Category { Temperature, Length, Weight };
aria::Property<Category> category{Category::Temperature};
aria::binding::Converter<Category, int> category_index{
    [](const Category& value) { return static_cast<int>(value); },
    {},
    [](const int& index) -> std::optional<Category> {
        if (index < 0 || index > 2) return std::nullopt;
        return static_cast<Category>(index);
    }
};
engine.bind_int_converted(category, adapter->view_for(categoryBox), category_index);
```

The converter defines the mapping; non-contiguous enum values need an explicit
table or switch. Invalid input retains the model value and reports through
the normal `binding.converter` diagnostics channel. Fixed-index bindings
observe `currentIndexChanged`, including programmatic index changes. Do not
bind index and stable ID to the same control at the same time.

### Changing options with stable IDs

`ObservableListModel` already supplies insertion, removal, move, replacement,
and label updates to a QComboBox through `setModel()`. The selection helper
adds the missing link between a model ID and the selected row:

```cpp
#include "aria/adapters/qt6/qt_combo_box_binding.hpp"
#include "aria/adapters/qt6/qt_list_model_adapter.hpp"

struct ThemeOption { std::string id; std::string label; };
aria::ObservableList<ThemeOption> options;
aria::Property<std::optional<std::string>> selected{std::string{"dark"}};

options.push_back(std::make_shared<ThemeOption>(ThemeOption{"light", "Light"}));
options.push_back(std::make_shared<ThemeOption>(ThemeOption{"dark", "Dark"}));
aria::adapters::qt6::ObservableListModel<ThemeOption> model{
    options, {{Qt::DisplayRole, "label"}, {Qt::UserRole, "id"}},
    [](const ThemeOption& option, int role) -> QVariant {
        if (role == Qt::DisplayRole) return QString::fromStdString(option.label);
        if (role == Qt::UserRole) return QString::fromStdString(option.id);
        return {};
    }};

QComboBox picker;
picker.setModel(&model);
auto selection_binding = aria::adapters::qt6::bind_combo_box_selection(
    selected, picker,
    [](const QVariant& data) { return data.toString().toStdString(); });

options.move(1, 0);        // "dark" remains selected, now at index 0
selected.set("light");  // select by identity, independently of its position
```

The default ID role is `Qt::UserRole`, so `currentData()` exposes the same
identity used by the binding. Pass another role as the last argument if the
model uses a different ID role. `Key` is inferred from
`Property<std::optional<Key>>`; the supplied callable converts item data into
that key type. String, integer, and enum IDs use the same helper.

| Change | Selection behavior |
|--------|--------------------|
| Initial binding / ViewModel write | The ViewModel ID determines the selected row. |
| `std::nullopt` | `currentIndex()` becomes `-1`; no row is selected. |
| ID not yet present | Display no selection and retain the pending ID; select it when it arrives. |
| Insert, move, or removal of another ID | Keep the selected ID; its index may change. |
| Label change / replacement retaining the ID | Keep selection, including duplicate display labels. |
| Selected ID disappears | Clear the selection to `std::nullopt`. |
| User chooses an option | Write its ID to the Property. |

IDs must be unique and stable within the model; labels may repeat or change
with localization. User input follows Qt's `activated` signal, which avoids
writing the control's automatic fallback selection into the ViewModel while
options change. Programmatic selection changes must go through the Property;
calling `setCurrentIndex()` alone does not change that Property. See the
[Qt signal contract](https://doc.qt.io/qt-6/qcombobox.html#signals).

Keep the returned Subscription alive, or pass it to `BindingEngine::adopt`.
Release disconnects both directions; destroying the widget first is safe.
The source, model, and Property must outlive the binding. Construct, update,
and release on the Qt GUI/graph thread. The helper supports non-editable,
single-column lists and one installed model; release and bind again after
replacing the model. Derived lists work through the same `ObservableListModel`.

---

## QtDispatcher — Event Loop Bridge

Connects Qt's event loop to Aria's `IDispatcher` interface:

```cpp
#include "aria/adapters/qt6/qt_dispatcher.hpp"

auto dispatcher = std::make_shared<aria::adapters::qt6::QtDispatcher>(&app);
```

| Method | Implementation |
|--------|---------------|
| `post(fn)` | `QMetaObject::invokeMethod(Qt::QueuedConnection)` |
| `post_delayed(ms, fn)` | `QTimer::singleShot(ms, fn)` |
| `is_main_thread()` | `QThread::currentThread() == context->thread()` |

Capabilities: `Post | Delay | MainThread | Autonomous` (Qt drives its own event loop, no manual pumping needed).

---

## Full Example

```cpp
#include "aria/adapters/qt6/qt_adapter.hpp"
#include "aria/adapters/qt6/qt_view.hpp"
#include "aria/adapters/qt6/qt_dispatcher.hpp"
#include "aria/binding/binding_engine.hpp"

class MainWindow : public QMainWindow {
public:
    MainWindow(QWidget* parent = nullptr) : QMainWindow(parent) {
        setupUi(this);

        auto disp = std::make_shared<aria::adapters::qt6::QtDispatcher>(this);
        aria::runtime::set_main_dispatcher(disp);

        engine_ = std::make_unique<aria::binding::BindingEngine>(
            adapter_, disp,
            aria::binding::BindingEngine::DispatchPolicy::SmartMarshal);

        // Wire up
        engine_->bind_text(vm_.query, adapter_->view_for(searchInput));
        engine_->bind_text_oneway(vm_.result, adapter_->view_for(resultLabel));
        engine_->bind_command(vm_.search_cmd, adapter_->view_for(searchBtn));
    }

private:
    SearchVm vm_;
    std::shared_ptr<aria::adapters::qt6::QtAdapter> adapter_ =
        std::make_shared<aria::adapters::qt6::QtAdapter>();
    std::unique_ptr<aria::binding::BindingEngine> engine_;
};
```

---

## See Also

- [View Binding →](../binding.md) — BindingEngine API reference
- [Collections →](../collections.md) — ObservableList and derived views
- [AriaTools →](https://github.com/dqsjqian/AriaTools) — flagship cross-platform application (Qt, iOS, Android; Web in progress)
