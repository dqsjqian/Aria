#pragma once

// C++20 coroutine Task<T>: a lazy, single-shot, awaitable that returns T (or void).
//
//   Task<int> compute() { co_return 42; }
//   Task<void> log()    { co_return; }
//
// The task is started when co_awaited (or by Task::start()).
// Exceptions thrown in the coroutine body are stored and rethrown by co_await.

#include <coroutine>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

namespace aria::async {

template<typename T = void>
class Task;

namespace detail {

template<typename T>
struct TaskPromiseBase {
    std::coroutine_handle<> continuation;
    std::exception_ptr exception;

    // Detached flag: when set (by Task::start_detached()), the promise's
    // FinalAwaiter destroys the coroutine frame at final_suspend instead
    // of transferring control to a `continuation`. This collapses the
    // older wrapper-coroutine + lambda-IIFE detach mechanism into a
    // single bit on the promise, and avoids GCC/MinGW codegen edge
    // cases around immediately-invoked lambda coroutines under -O2.
    bool detached = false;

    auto initial_suspend() noexcept { return std::suspend_always{}; }

    struct FinalAwaiter {
        bool await_ready() const noexcept { return false; }

        // Templated to accept any coroutine_handle whose promise inherits TaskPromiseBase<T>
        template<typename Promise>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) noexcept {
            TaskPromiseBase<T>& base = h.promise();
            if (base.detached) {
                // Self-destroy: the original Task object is gone, no one
                // is awaiting the result. Any captured exception was
                // already swallowed by `unhandled_exception` into
                // `base.exception`; the detached path intentionally
                // ignores it (matches the legacy wrapper's `catch (...)`).
                h.destroy();
                return std::noop_coroutine();
            }
            return base.continuation ? base.continuation : std::noop_coroutine();
        }

        void await_resume() const noexcept {}
    };

    auto final_suspend() noexcept { return FinalAwaiter{}; }

    void unhandled_exception() noexcept { exception = std::current_exception(); }
};

template<typename T>
struct TaskPromise : TaskPromiseBase<T> {
    using value_type = T;
    std::optional<T> value;

    Task<T> get_return_object() noexcept;

    template<typename U>
    void return_value(U&& v) noexcept(std::is_nothrow_constructible_v<T, U>) {
        value.emplace(std::forward<U>(v));
    }
};

template<>
struct TaskPromise<void> : TaskPromiseBase<void> {
    using value_type = void;
    Task<void> get_return_object() noexcept;
    void return_void() noexcept {}
};

}  // namespace detail

template<typename T>
class [[nodiscard]] Task {
public:
    using promise_type = detail::TaskPromise<T>;
    using handle_type = std::coroutine_handle<promise_type>;

    Task() noexcept = default;
    explicit Task(handle_type h) noexcept : handle_(h) {}

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    Task(Task&& o) noexcept : handle_(std::exchange(o.handle_, {})) {}
    Task& operator=(Task&& o) noexcept {
        if (this != &o) {
            if (handle_) handle_.destroy();
            handle_ = std::exchange(o.handle_, {});
        }
        return *this;
    }

    ~Task() {
        if (handle_) handle_.destroy();
    }

    [[nodiscard]] bool done() const noexcept { return handle_ && handle_.done(); }

    /// Make the task awaitable.
    /// IMPORTANT: For a temporary `co_await Task<T>{h}` we must MOVE the
    /// handle into the awaiter, not copy it.  Otherwise the temporary Task's
    /// destructor calls handle.destroy() while the coroutine is still
    /// executing → UAF.  See cpp20-coroutine-pitfalls #9.
    auto operator co_await() && noexcept {
        struct Awaiter {
            handle_type h;

            bool await_ready() const noexcept { return !h || h.done(); }

            std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
                h.promise().continuation = caller;
                return h;
            }

            T await_resume() {
                auto& p = h.promise();
                if (p.exception) std::rethrow_exception(p.exception);
                if constexpr (!std::is_void_v<T>) {
                    return std::move(*p.value);
                }
            }

            // Awaiter takes ownership: destroy when done.
            ~Awaiter() { if (h) h.destroy(); }

            Awaiter(handle_type hh) noexcept : h(hh) {}
            Awaiter(Awaiter&& o) noexcept : h(std::exchange(o.h, {})) {}
            Awaiter& operator=(Awaiter&&) = delete;
            Awaiter(const Awaiter&) = delete;
            Awaiter& operator=(const Awaiter&) = delete;
        };
        // Transfer ownership from the temporary `*this` to the Awaiter.
        return Awaiter{std::exchange(handle_, {})};
    }

    /// Eagerly start the task without awaiting it. Use blocking_get() if you need the result.
    void start() {
        if (handle_ && !handle_.done()) handle_.resume();
    }

    /// Start the task and detach it: the coroutine frame stays alive until
    /// the coroutine completes, even after this Task object is destroyed.
    /// The frame is automatically destroyed on completion — no leaks.
    ///
    /// Implementation: we set `promise.detached = true` and resume the
    /// frame. When the coroutine reaches `final_suspend`, the templated
    /// `FinalAwaiter::await_suspend` observes the flag and calls
    /// `h.destroy()` itself, releasing the frame and all of its locals
    /// (captures, awaiters, etc.) in a single step.
    ///
    /// This replaces an earlier wrapper-coroutine + lambda-IIFE design.
    /// That design was correct on Apple Clang and libc++ but tripped a
    /// GCC/MinGW UCRT64 codegen edge case (heap corruption on process
    /// exit) under `-O2 Release`. The flag-based approach has no
    /// wrapper frame, no awaiter destructor chain to unwind on the
    /// detach path, and is the canonical "self-destroying coroutine"
    /// pattern used by `cppcoro::task` and friends.
    void start_detached() && {
        if (!handle_) return;
        auto h = std::exchange(handle_, {});
        h.promise().detached = true;
        // Resume the frame. If the body is fully synchronous, this call
        // will run it to completion; the frame self-destroys inside
        // `FinalAwaiter::await_suspend` and `h` is dangling on return
        // (we never touch it again). If the body suspends mid-flight,
        // this call returns with `h` still alive; whoever resumes the
        // suspension point later will eventually drive it to
        // `final_suspend`, which then self-destroys.
        h.resume();
    }

    /// Lvalue convenience used internally by AsyncCommand.
    void start_detached_() { std::move(*this).start_detached(); }

    /// Blocking accessor — only safe if the task body is synchronous (no real async).
    /// For real async use co_await or a Scheduler.
    T blocking_get() {
        if (!handle_) throw std::runtime_error("Task: empty handle");
        if (!handle_.done()) handle_.resume();
        if (!handle_.done()) throw std::runtime_error("Task: did not complete synchronously");
        auto& p = handle_.promise();
        if (p.exception) std::rethrow_exception(p.exception);
        if constexpr (!std::is_void_v<T>) {
            return std::move(*p.value);
        }
    }

private:
    handle_type handle_{};
};
namespace detail {

template<typename T>
Task<T> TaskPromise<T>::get_return_object() noexcept {
    return Task<T>{std::coroutine_handle<TaskPromise<T>>::from_promise(*this)};
}

inline Task<void> TaskPromise<void>::get_return_object() noexcept {
    return Task<void>{std::coroutine_handle<TaskPromise<void>>::from_promise(*this)};
}

}  // namespace detail

}  // namespace aria::async
