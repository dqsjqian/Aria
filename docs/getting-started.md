# Getting started

## Build

```bash
cmake -B build/flavors/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/flavors/release -j
ctest --test-dir build
```

The first configure pulls [doctest](https://github.com/doctest/doctest) via
the bundled `cmake/CPM.cmake`. After that everything is offline.

## Hello, Property

```cpp
#include "aria/aria.hpp"
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
    [&]{ /* ...sign user out... */ },
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
    /* react to Insert / Remove / ItemChanged / Reset */
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

if (!v.result().get().valid) {
    for (auto& err : v.result().get().errors) std::cerr << err << "\n";
}
```

## Async work with coroutines

```cpp
#include "aria/async/task.hpp"
#include "aria/async/executor.hpp"
using namespace aria::async;

ThreadPoolExecutor pool(4);

Task<int> compute_total(std::vector<int> xs) {
    co_await schedule_on(pool);            // jump to worker thread
    int sum = 0;
    for (auto x : xs) sum += x;
    co_return sum;
}

int main() {
    auto t = compute_total({1, 2, 3, 4});
    std::cout << t.blocking_get() << "\n";  // 10
}
```

For UI applications, hop back to the main thread before touching view-model
properties:

```cpp
co_await schedule_on(main_dispatcher_executor);
greeting.set("Done!");
```

## Wiring a real ViewModel

```cpp
class GreetingViewModel : public binding::ViewModel {
public:
    Property<std::string> name{"World"};
    Computed<std::string> greeting{
        [this]{ return "Hello, " + name.get() + "!"; }
    };
    Command<> reset{[this]{ name = "World"; }};
};
```

```cpp
auto adapter = std::make_shared<MyQtAdapter>(); // platform-specific
binding::BindingEngine engine(adapter);

GreetingViewModel vm;
engine.bind_text(vm.name, my_qline_edit);
engine.bind_text_oneway(vm.greeting, my_qlabel);
engine.bind_command(vm.reset, my_qpushbutton);
```

That's it — you have a full MVVM application that can be re-bound to
SwiftUI, Jetpack Compose, or HTML/DOM by swapping the adapter.

## Next steps

- Read [`docs/architecture.md`](architecture.md) for the layering rationale.
- Explore [AriaTools](https://github.com/dqsjqian/AriaTools), the flagship cross-platform application for Qt, iOS, Android, and Web.
- Browse `tests/acceptance/` for executable framework contracts and the focused snippets in these guides for individual concepts.
- Run the benchmark: `./build/flavors/release/benchmark/aria_bench`.
