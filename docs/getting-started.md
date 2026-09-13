# Getting started

## Build

Aria supports C++23; C++20 remains the default and minimum. The default build:

```bash
cmake -B build/flavors/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/flavors/release -j
ctest --test-dir build/flavors/release --output-on-failure
```

To select C++23 explicitly, add `-DCMAKE_CXX_STANDARD=23` when configuring:

```bash
cmake -B build/flavors/cxx23 -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=23
cmake --build build/flavors/cxx23 -j
ctest --test-dir build/flavors/cxx23 --no-tests=error --output-on-failure
```

The repository includes doctest, so the normal test build uses the bundled
header. If that vendored header is absent, CMake fetches the fallback test
dependency through `cmake/CPM.cmake`.

## Use Aria in an application

Install the build into a prefix of your choice, for example from the Aria
repository root:

```bash
cmake --install build/flavors/release --prefix "$PWD/aria-install"
```

In a separate application directory, save the next example as `main.cpp` and
create this `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(greeting LANGUAGES CXX)
find_package(aria 2.0 CONFIG REQUIRED COMPONENTS core)
add_executable(greeting main.cpp)
target_link_libraries(greeting PRIVATE aria::core)
```

Configure the application with the absolute path to that installation:

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=/absolute/path/to/Aria/aria-install
cmake --build build
```

The imported target supplies the C++20 minimum. You can add
`-DCMAKE_CXX_STANDARD=23` to the application's configure command to select
C++23. The `aria/aria.hpp` umbrella contains the core API; async and platform
adapters have their own headers and CMake targets.

## Hello, Property

```cpp
#include "aria/aria.hpp"
#include <functional>
#include <iostream>
#include <string>
using namespace aria;

int main() {
    Property<std::string> name{"World"};

    auto sub = name.bind([](const std::string& v) {
        std::cout << "Hello, " << v << "!\n";
    });
    // → "Hello, World!"

    name = "Alice";  // → "Hello, Alice!"
    name = "Bob";    // → "Hello, Bob!"
}
```

`bind()` invokes the callback once with the current value and again on every
change. The returned `Subscription` is RAII — let it drop out of scope to
disconnect.

The next four snippets each replace the body of `main()` in this program;
keep its includes and `using namespace aria;` declaration.

## Computed (derived) properties

```cpp
Property<int> a{3}, b{4};

// No explicit dependency list needed — the engine auto-tracks every
// Property read inside the lambda on first evaluation.
Computed<int> sum([&]{ return a.get() + b.get(); });

std::cout << sum.get() << "\n";  // 7
a = 10;
std::cout << sum.get() << "\n";  // 14
```

## Commands with CanExecute

```cpp
Property<bool> logged_in{false};

Command<> logout(
    []{ std::cout << "Signed out\n"; },
    [&]{ return logged_in.get(); }       // can_execute predicate
);

logout.execute();   // no-op (not logged in)
logged_in = true;
logout.execute();   // runs
```

A button can be bound to the command via `BindingEngine::bind_command()` —
the button's `enabled` state automatically tracks `can_execute`.

## ObservableList

```cpp
struct Todo {
    Property<bool> done{false};
    [[nodiscard]] Subscription on_changed(std::function<void(const Todo&)> fn) {
        return done.on_changed([this, fn](bool) { fn(*this); });
    }
};

ObservableList<Todo> list;
auto sub = list.observe([](const ListChange<Todo>& c) {
    if (c.kind == ListChangeKind::Insert) std::cout << "Inserted\n";
    if (c.kind == ListChangeKind::ItemChanged) std::cout << "Item changed\n";
    if (c.kind == ListChangeKind::Remove) std::cout << "Removed\n";
});

auto t = list.emplace_back();   // → Insert
t->done = true;                  // → ItemChanged
list.remove_at(0);               // → Remove
```

The `ItemChanged` notification is automatic if `T` provides an `on_changed`
member that returns a `Subscription`.

## Validation

```cpp
Property<std::string> email{""};
Validator<std::string> v(email);
v.must([](auto& s){ return !s.empty(); }, "Email is required")
 .must([](auto& s){ return s.find('@') != std::string::npos; }, "Email must contain @");

const auto& state = v.state().get();
if (!state.valid) {
    for (const auto& error : state.errors) {
        std::cerr << error.message << "\n";
    }
}
```

## Async work with coroutines

For this complete console program, change the CMake component to `async` and
link `aria::async`. A future carries the result back to the console thread,
while `CoroutineScope` owns the running task.

```cpp
#include "aria/async/task.hpp"
#include "aria/async/executor.hpp"
#include "aria/async/scope.hpp"
#include <exception>
#include <future>
#include <iostream>
#include <utility>
#include <vector>
using namespace aria::async;

Task<int> compute_total(IExecutor& worker, std::vector<int> xs) {
    co_await schedule_on(worker);
    int sum = 0;
    for (auto x : xs) sum += x;
    co_return sum;
}

Task<void> complete_total(IExecutor& worker, std::vector<int> xs,
                          std::promise<int> completion) {
    try {
        const int total = co_await compute_total(worker, std::move(xs));
        completion.set_value(total);
    } catch (...) {
        completion.set_exception(std::current_exception());
    }
}

int main() {
    ThreadPoolExecutor pool{2};  // outlives the scope and its tasks
    CoroutineScope scope;
    std::promise<int> completion;
    auto result = completion.get_future();
    scope.launch_simple(complete_total(pool, {1, 2, 3, 4}, std::move(completion)));

    int status = 0;
    try {
        std::cout << result.get() << "\n";  // waits for the result: 10
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        status = 1;
    }
    if (!scope.cancel_and_join()) return 1;
    return status;
}
```

The promise and input vector are owned by coroutine parameters; the worker
reference remains valid until the tasks finish. `blocking_get()` only supports
tasks that finish during a single synchronous resume, so it cannot wait for
this thread-pool operation.

In a GUI application, keep the UI event loop running and dispatch view-model
writes back to its owner thread. Do not block that thread waiting for work
that needs the same event loop. See the [async guide](guide/async.md) and
[Qt guide](guide/adapters/qt6.md) for executor and dispatcher integration.

## Wiring a real ViewModel

Enable the Qt adapter when building Aria (`-DARIA_BUILD_QT6=ON`), then rebuild
and reinstall it. CMake must be able to find your Qt 6 Core and Widgets
installation. For the application, use
`find_package(aria 2.0 CONFIG REQUIRED COMPONENTS qt6)` and link `aria::qt6`
in the CMake file above.

This complete Qt Widgets program binds a text field, a derived label, and a
reset button:

```cpp
#include "aria/aria.hpp"
#include "aria/binding/binding_engine.hpp"
#include "aria/binding/view_model.hpp"
#include "aria/adapters/qt6/qt_adapter.hpp"
#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>
#include <memory>
#include <string>
using namespace aria;

class GreetingViewModel final : public binding::ViewModel {
public:
    Property<std::string> name{"World"};
    Computed<std::string> greeting{
        [this]{ return "Hello, " + name.get() + "!"; }
    };
    Command<> reset{[this]{ name = "World"; }};
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    GreetingViewModel vm;
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* name_edit = new QLineEdit(&window);
    auto* greeting_label = new QLabel(&window);
    auto* reset_button = new QPushButton("Reset", &window);
    layout->addWidget(name_edit);
    layout->addWidget(greeting_label);
    layout->addWidget(reset_button);

    auto adapter = std::make_shared<adapters::qt6::QtAdapter>();
    binding::BindingEngine engine(adapter);
    engine.bind_text(vm.name, adapter->view_for(name_edit));
    engine.bind_text_oneway(vm.greeting, adapter->view_for(greeting_label));
    engine.bind_command(vm.reset, adapter->view_for(reset_button));

    window.show();
    return app.exec();
}
```

`BindingEngine` binds an `IView&`. `QtAdapter::view_for()` supplies and owns
the wrapper for each native widget; the returned reference remains valid
until that widget or the adapter is destroyed. Qt owns the child widgets,
and the declaration order above destroys the engine before the adapter,
widgets, and view-model.

This example performs all updates on the Qt UI thread. Other implemented
adapters can reuse the view-model with their own native view wiring.
SwiftUI integration remains conditional work in the [roadmap](ROADMAP.md).

## Next steps

- Read [`docs/architecture.md`](architecture.md) for the layering rationale.
- Read the [Qt guide](guide/adapters/qt6.md) for more widget bindings and UI dispatch.
- Browse `tests/acceptance/` for executable framework contracts and the focused snippets in these guides for individual concepts.
- With `ARIA_BUILD_BENCHMARK=ON`, run a benchmark such as `./build/flavors/release/bin/aria_bench_command` (multi-config generators add a configuration directory such as `bin/Release/`).
