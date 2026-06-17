#pragma once

// Helpers that hide the C++20 "no co_await in catch" rule and the
// "must hop back to UI thread before mutating" rule.
//
//   on_ui(ui_exec, []() -> Task<R> { ... });          // run, end on UI thread
//   on_ui_safe(ui, body, on_error);                   // also routes errors
//
// Inside the lambda you can `co_await` freely; if you throw, the error
// handler is called on the UI thread (or rethrown if no handler).

#include "aria/async/executor.hpp"
#include "aria/async/task.hpp"

#include <exception>
#include <functional>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace aria::async {

namespace detail {

template<typename Fn>
using body_result_t = typename std::invoke_result_t<Fn>::promise_type::value_type;

}  // namespace detail

/// Run `body()` (a coroutine factory). Always end suspended on `ui_exec`
/// before returning. Exceptions are rethrown from the returned Task.
template<typename Fn>
auto on_ui(IExecutor& ui_exec, Fn body) -> Task<detail::body_result_t<Fn>> {
    using R = detail::body_result_t<Fn>;
    std::exception_ptr ex;

    if constexpr (std::is_void_v<R>) {
        try { co_await body(); } catch (...) { ex = std::current_exception(); }
        co_await schedule_on(ui_exec);
        if (ex) std::rethrow_exception(ex);
    } else {
        std::optional<R> result;
        try { result.emplace(co_await body()); } catch (...) { ex = std::current_exception(); }
        co_await schedule_on(ui_exec);
        if (ex) std::rethrow_exception(ex);
        co_return std::move(*result);
    }
}

/// Like on_ui(), but routes any exception to `on_error(e)` on the UI thread.
/// Always returns Task<void> (the success value, if any, is discarded —
/// use the success path of `body` to update Properties directly).
template<typename Body, typename OnError>
Task<void> on_ui_safe(IExecutor& ui_exec, Body body, OnError on_error) {
    std::exception_ptr ex;
    try {
        co_await body();
    } catch (...) {
        ex = std::current_exception();
    }
    co_await schedule_on(ui_exec);
    if (ex) {
        try { std::rethrow_exception(ex); }
        catch (const std::exception& e) { on_error(e); }
        catch (...) {
            std::runtime_error generic("unknown exception");
            on_error(generic);
        }
    }
}

}  // namespace aria::async
