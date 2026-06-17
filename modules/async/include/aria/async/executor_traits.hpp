#pragma once

// Executor traits — declare which executors are safe to use in which
// roles, so that components like `AsyncCommand` can statically reject
// unsafe combinations.
//
// Two orthogonal capabilities are encoded:
//
//   is_safe_graph_executor_v<E>
//     E may be passed as the **graph-thread (UI) executor**. Calls to
//     `co_await schedule_on(ui)` from a worker thread must end up
//     resuming on the executor's owner thread (the thread that owns
//     the reactive graph), NOT on the calling worker thread.
//
//   is_safe_worker_executor_v<E>
//     E may be passed as the **worker executor**. Worker tasks may run
//     on any thread (typically a thread pool), as long as the worker
//     never directly writes reactive state — that is the contract that
//     `AsyncCommand` enforces by always hopping back to the graph
//     executor before touching `Property<T>`.
//
// Default: both are `false`. Custom executors must opt in by
// specialising the trait or by inheriting from the marker base classes
// declared below (CRTP-style markers are deliberately avoided so that
// users can also retrofit traits onto third-party executor classes).
//
// To opt in for an external type:
//
//   namespace aria::async {
//       template<> struct is_safe_graph_executor<MyMainLoop> : std::true_type {};
//   }

#include "aria/async/executor.hpp"

#include <type_traits>

namespace aria::async {

// ── primary trait templates (default: false) ──────────────────────────────
template<typename E> struct is_safe_graph_executor  : std::false_type {};
template<typename E> struct is_safe_worker_executor : std::false_type {};

template<typename E>
inline constexpr bool is_safe_graph_executor_v = is_safe_graph_executor<E>::value;
template<typename E>
inline constexpr bool is_safe_worker_executor_v = is_safe_worker_executor<E>::value;

// ── built-in executor specialisations ─────────────────────────────────────

// InlineExecutor: safe for both roles ONLY when both ui and worker are
// inline (single-threaded scenarios). The combination of "InlineExecutor
// graph + multi-threaded worker" is rejected by `AsyncCommand` via a
// dedicated static_assert; see `async_command.hpp`.
template<> struct is_safe_graph_executor<InlineExecutor>  : std::true_type {};
template<> struct is_safe_worker_executor<InlineExecutor> : std::true_type {};

// ThreadPoolExecutor: worker only. Property writes from a pool thread
// would race the reactive graph's owner-thread invariant.
template<> struct is_safe_worker_executor<ThreadPoolExecutor> : std::true_type {};

// MainThreadExecutor: safe for both. Owner-thread is bound on first
// pump and asserted thereafter, which is exactly the reactive graph's
// expectation.
template<> struct is_safe_graph_executor<MainThreadExecutor>  : std::true_type {};
template<> struct is_safe_worker_executor<MainThreadExecutor> : std::true_type {};

// ── concept aliases ──────────────────────────────────────────────────────
template<typename E>
concept SafeGraphExecutor = std::is_base_of_v<IExecutor, E>
                         && is_safe_graph_executor_v<E>;

template<typename E>
concept SafeWorkerExecutor = std::is_base_of_v<IExecutor, E>
                          && is_safe_worker_executor_v<E>;

}  // namespace aria::async
