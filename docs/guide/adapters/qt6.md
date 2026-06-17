# Qt6 Adapter

Integrating Aria with Qt 6.x desktop applications. The Qt6 adapter provides `QtAdapter` (widget binding), `QtView` (view wrapper), `ObservableListModel` (list binding), and `QtDispatcher` (event loop bridge).

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

aria::adapters::qt6::QtAdapter adapter;
aria::binding::BindingEngine engine(adapter,
    aria::binding::DispatchPolicy::SmartMarshal);
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

### Text (QLabel / QLineEdit / QTextEdit)

```cpp
aria::Property<std::string> name{"Alice"};

// Two-way: user typing → Property, Property → widget text
auto line_edit_view = std::make_shared<aria::adapters::qt6::QtView>(lineEdit);
engine.bind_text(name, *line_edit_view);

// One-way: Property → QLabel display only
auto label_view = std::make_shared<aria::adapters::qt6::QtView>(label);
engine.bind_text_oneway(name, *label_view);
```

### Bool (QCheckBox / QRadioButton / QToggleButton)

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
    vm.tasks_shared(), [](const auto& t) { return !t->done; });

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

If the source list mutates from a background thread, the model automatically queues the change to the Qt event loop via `QMetaObject::invokeMethod`. Items are resolved at emit time to prevent stale reads.

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
            adapter_, aria::binding::DispatchPolicy::SmartMarshal);

        // Wire up
        engine_->bind_text(vm_.query,
            *std::make_shared<aria::adapters::qt6::QtView>(searchInput));
        engine_->bind_text_oneway(vm_.result,
            *std::make_shared<aria::adapters::qt6::QtView>(resultLabel));
        engine_->bind_command(vm_.search_cmd,
            *std::make_shared<aria::adapters::qt6::QtView>(searchBtn));
    }

private:
    aria::adapters::qt6::QtAdapter adapter_;
    std::unique_ptr<aria::binding::BindingEngine> engine_;
    SearchVm vm_;
};
```

---

## See Also

- [View Binding →](../binding.md) — BindingEngine API reference
- [Collections →](../collections.md) — ObservableList and derived views
- [Demo 1 — Qt Showcase →](../../../examples/1-qt-showcase/)
