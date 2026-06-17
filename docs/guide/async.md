# Async & Coroutines

Aria's async layer provides C++20 coroutine-based primitives for asynchronous work, built on top of the reactive graph. The key types:

- **`Task<T>`** — lazy, single-shot coroutine awaitable
- **`AsyncCommand<R, Args...>`** — three-state async action (executing / error / result)
- **`CoroutineScope` / `ViewModelScope`** — structured concurrency tied to a lifetime
- **`CancellationToken`** — cooperative cancellation
- **Combinators** — `when_all`, `when_any`, `with_timeout`

**Include:** `#include "aria/async/task.hpp"`, `#include "aria/async/async_command.hpp"`, etc.

---

## Task\<T\>

`Task<T>` is a lazy coroutine — it does nothing until `co_await`ed.

### Basic Usage

```cpp
aria::async::Task<int> compute_value() {
    co_return 42;
}

aria::async::Task<void> show_result() {
    int val = co_await compute_value();
    std::cout << "Got: " << val << "\n";
}
```

### Void Specialization

```cpp
aria::async::Task<void> log_message(std::string msg) {
    std::cout << msg << "\n";
    co_return;
}
```

### Exception Handling

Exceptions thrown inside the coroutine body are stored and re-thrown at the `co_await` site:

```cpp
aria::async::Task<int> risky() {
    throw std::runtime_error("boom");
    co_return 0;  // unreachable
}

aria::async::Task<void> caller() {
    try {
        int v = co_await risky();
    } catch (const std::runtime_error& e) {
        // Caught here
    }
}
```

### Fire-and-Forget (Detached)

```cpp
aria::async::Task<void> background_work() {
    // Long-running work...
    co_return;
}

// Start without awaiting — runs independently
background_work().start_detached();
```

> **Warning:** Detached tasks have no lifetime guard. Ensure captured references outlive the coroutine.

---

## CancellationToken / CancellationSource

Cooperative cancellation. A `CancellationSource` owns the flag; a `CancellationToken` is a read-only view.

```cpp
aria::async::CancellationSource src;
aria::async::CancellationToken tok = src.token();

tok.is_cancelled();   // false
src.cancel();
tok.is_cancelled();   // true
```

### Inside a Coroutine

```cpp
aria::async::Task<void> poll_loop(aria::async::CancellationToken tok) {
    while (!tok.is_cancelled()) {
        co_await aria::async::sleep_for(100ms);
        tok.throw_if_cancelled();  // throws OperationCancelled
        do_work();
    }
}
```

---

## Executor

Executors abstract scheduling — where coroutines run.

```cpp
#include "aria/async/executor.hpp"

// Typically provided by the platform adapter
aria::async::IExecutor& ui;      // main/UI thread
aria::async::IExecutor& worker;  // background thread pool
```

### Schedule On

Hop between executors:

```cpp
aria::async::Task<UserProfile> load_profile(int uid) {
    co_await aria::async::schedule_on(worker);  // jump to background
    auto data = fetch_from_db(uid);              // blocking OK here
    co_await aria::async::schedule_on(ui);       // hop back to UI
    co_return data;
}
```

---

## AsyncCommand\<R, Args...\>

An async command exposes three reactive properties that the UI can bind to:

| Property | Type | Meaning |
|----------|------|---------|
| `is_executing` | `Property<bool>` | True while any invocation is in flight |
| `last_error` | `Property<std::optional<Error>>` | Most recent error, `nullopt` when OK |
| `last_result` | `Property<std::optional<R>>` | Most recent successful result (R ≠ void) |

### Basic Usage

```cpp
#include "aria/async/async_command.hpp"

aria::async::AsyncCommand<SearchResult, std::string> search{ui, worker,
    [](std::string query) -> aria::async::Task<SearchResult> {
        co_await aria::async::schedule_on(worker);
        co_return perform_search(query);
    }
};

// Trigger from UI
search.execute("hello");

// Bind in UI
search.is_executing.bind([](bool running) {
    spinner.set_visible(running);
});
```

### Cancellable Action

Accept a `CancellationToken` as the first parameter:

```cpp
aria::async::AsyncCommand<Data, int> fetch{ui, worker,
    [](aria::async::CancellationToken tok, int id) -> aria::async::Task<Data> {
        co_await aria::async::schedule_on(worker);
        tok.throw_if_cancelled();
        co_return heavy_load(id);
    }
};
```

### Concurrency Policies

```cpp
#include "aria/async/async_command.hpp"

// Parallel (default): multiple invocations run concurrently
aria::async::AsyncCommand<void, std::string> cmd_parallel{
    ui, worker, action, {}, aria::async::AsyncCommandPolicy::Parallel};

// LatestOnly: new execute() cancels in-flight work (search-as-you-type)
aria::async::AsyncCommand<Results, std::string> cmd_latest{
    ui, worker, action, {}, aria::async::AsyncCommandPolicy::LatestOnly};

// DropIfRunning: ignore execute() while busy (prevent double-submit)
aria::async::AsyncCommand<void> cmd_drop{
    ui, worker, action, {}, aria::async::AsyncCommandPolicy::DropIfRunning};
```

### Inside a ViewModel

```cpp
class SearchVm : public aria::binding::ViewModel {
public:
    SearchVm(aria::async::IExecutor& ui, aria::async::IExecutor& worker)
        : search(ui, worker,
            [this](std::string q) -> aria::async::Task<Result> {
                co_await aria::async::schedule_on(worker);
                co_return do_search(q);
            })
    {
        scope_.attach(*this);
    }

    aria::Property<std::string> query{""};
    aria::async::AsyncCommand<Result, std::string> search;

private:
    aria::binding::ViewModelScope scope_;
};
```

---

## ViewModelScope

Ties `CoroutineScope` to a ViewModel's lifetime. Destroying the VM cancels and joins all in-flight coroutines.

```cpp
#include "aria/binding/view_model_scope.hpp"

class PollingVm : public aria::binding::ViewModel {
public:
    PollingVm() { scope_.attach(*this); }

    void start() {
        scope_.launch([this](aria::async::CancellationToken tok) -> aria::async::Task<void> {
            while (!tok.is_cancelled()) {
                co_await aria::async::sleep_for(1s);
                tok.throw_if_cancelled();
                refresh();
            }
        });
    }

private:
    aria::binding::ViewModelScope scope_;
};
```

When `PollingVm` is destroyed, `scope_` calls `cancel_and_join()` (default 5 s timeout). Stuck coroutines are reported as leaks.

---

## Combinators

### when_all

Wait for all tasks to complete:

```cpp
aria::async::Task<void> load_all() {
    auto [users, posts, comments] = co_await aria::async::when_all(
        load_users(),
        load_posts(),
        load_comments()
    );
}
```

### when_any

Complete when the first task finishes (others are cancelled):

```cpp
aria::async::Task<Response> race_servers() {
    co_return co_await aria::async::when_any(
        fetch_from_primary(),
        fetch_from_backup()
    );
}
```

### with_timeout

Abort if a task exceeds the deadline:

```cpp
aria::async::Task<Data> fetch_with_deadline() {
    co_return co_await aria::async::with_timeout(
        slow_fetch(),
        std::chrono::seconds(5)
    );
}
```

---

## Quick Reference

| Type | Purpose | Produces Value |
|------|---------|----------------|
| `Task<T>` | Lazy coroutine | Yes (`T`) |
| `AsyncCommand<R, Args...>` | Three-state async action | Via `last_result` |
| `CoroutineScope` | Launch + cancel coroutines | No |
| `ViewModelScope` | Scope tied to VM lifetime | No |
| `CancellationToken` | Cooperative cancellation check | No |
| `CancellationSource` | Cancel producer | No |
| `IExecutor` | Where to run | No |

---

## See Also

- [ViewModel →](viewmodel.md) — `ViewModelScope` in context
- [Reactive Core →](reactive-core.md) — `Property` that `AsyncCommand` exposes
- [Lifecycle & Threading →](../reference/lifecycle.md) — thread-affinity contract for async
