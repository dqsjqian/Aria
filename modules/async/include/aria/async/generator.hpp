#pragma once

// Generator<T>: pull-based C++20 coroutine.
//
//   Generator<int> fib(int n) {
//       int a = 0, b = 1;
//       for (int i = 0; i < n; ++i) {
//           co_yield a;
//           int t = a + b; a = b; b = t;
//       }
//   }
//
//   for (int x : fib(10)) std::cout << x << ' ';
//
// The generator is lazy — values are produced one at a time on demand.
//
// ⚠️  CRITICAL: do NOT make the body a lambda that captures by reference.
// Coroutine frames live on the heap, but the lambda object expires at the
// call expression, leaving dangling references. Use a free function, a
// member function, or pass state through parameters.

#include <coroutine>
#include <exception>
#include <iterator>
#include <utility>

namespace aria::async {

template<typename T>
class Generator {
public:
    struct promise_type {
        T current_value;
        std::exception_ptr exception;

        Generator get_return_object() noexcept {
            return Generator{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }

        std::suspend_always yield_value(T value) noexcept {
            current_value = std::move(value);
            return {};
        }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { exception = std::current_exception(); }

        // Disallow co_await inside a Generator
        template<typename U>
        std::suspend_never await_transform(U&&) = delete;
    };

    using handle_type = std::coroutine_handle<promise_type>;

    Generator() = default;
    explicit Generator(handle_type h) noexcept : h_(h) {}

    Generator(const Generator&) = delete;
    Generator& operator=(const Generator&) = delete;
    Generator(Generator&& o) noexcept : h_(std::exchange(o.h_, {})) {}
    Generator& operator=(Generator&& o) noexcept {
        if (this != &o) {
            if (h_) h_.destroy();
            h_ = std::exchange(o.h_, {});
        }
        return *this;
    }
    ~Generator() { if (h_) h_.destroy(); }

    // ── Iterator API (range-for compatible) ──────────────────────
    class Iterator {
        handle_type h_;
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using reference = const T&;
        using pointer = const T*;

        Iterator() noexcept = default;
        explicit Iterator(handle_type h) noexcept : h_(h) {}

        Iterator& operator++() {
            h_.resume();
            if (h_.done()) {
                if (auto e = h_.promise().exception) std::rethrow_exception(e);
                h_ = nullptr;
            }
            return *this;
        }
        void operator++(int) { ++*this; }

        const T& operator*()  const noexcept { return h_.promise().current_value; }
        const T* operator->() const noexcept { return &h_.promise().current_value; }

        bool operator==(std::default_sentinel_t) const noexcept { return !h_ || h_.done(); }
    };

    Iterator begin() {
        if (!h_) return Iterator{};
        h_.resume();
        if (h_.done()) {
            if (auto e = h_.promise().exception) std::rethrow_exception(e);
            return Iterator{};
        }
        return Iterator{h_};
    }
    std::default_sentinel_t end() noexcept { return {}; }

private:
    handle_type h_{};
};

}  // namespace aria::async
