#pragma once

// aria::function_ref<R(Args...)> — non-owning, zero-allocation, signature-erased
// callable handle.
//
// Inspired by `std::function_ref` (C++26 / P0792). A `function_ref` is a thin
// view over a callable: it stores **two pointers** (target + invoker) and
// nothing else. It does **not** own its target; the caller is responsible for
// keeping the underlying object alive for as long as the `function_ref` is in
// use.
//
// When to reach for it
// --------------------
// * Hot-path callbacks that are invoked synchronously and never escape the
//   call site (e.g. predicates passed to STL-like algorithms, visitors,
//   "do-this-once and return" callbacks).
// * Public APIs that want to accept any callable without forcing a
//   `std::function` allocation on every call. Compare:
//
//       // before — every caller pays for a std::function copy + possible heap.
//       void for_each(const std::function<void(int)>& fn);
//
//       // after  — function_ref is two pointers; no allocation, no virtuals.
//       void for_each(aria::function_ref<void(int)> fn);
//
// When NOT to use it
// ------------------
// * If the callback is going to be **stored** past the call (e.g. registered
//   as an observer, captured by a coroutine, queued onto a dispatcher), use
//   an owning type instead — `aria::inplace_function` for small lambdas,
//   `std::function` if the size cap is unacceptable.
// * `function_ref` does NOT participate in copy/move of the underlying
//   callable. Mutating captures inside the wrapped lambda will mutate the
//   original captured-by-value object — correct behaviour, but easy to
//   misread when comparing with `std::function`.
//
// Design notes
// ------------
// * The invoker is a free function pointer (not a virtual call). Modern
//   compilers reliably inline through it when both the construction site and
//   the call site are visible.
// * Construction from a function pointer (e.g. `int(*)(int)`) is supported
//   directly, including via implicit decay from a function reference.
// * Construction from a member function or a callable wrapping `nullptr` is
//   intentionally rejected at compile time.
// * `function_ref` is trivially copyable, so copying it is free.
// * Disengaged ("default constructed") `function_ref` invokes UB if called;
//   the type contract treats default construction as a placeholder for later
//   assignment, mirroring `string_view`.

#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace aria {

template<class Sig>
class function_ref;  // primary template intentionally undefined

template<class R, class... Args>
class function_ref<R(Args...)> {
public:
    using result_type = R;

    // Default-constructed function_ref is disengaged. Calling it is UB.
    // Provided for the "construct now, assign later" idiom.
    constexpr function_ref() noexcept = default;

    constexpr function_ref(std::nullptr_t) noexcept {}

    // Construct from a free function pointer.
    function_ref(R (*fp)(Args...)) noexcept
        : obj_(reinterpret_cast<const void*>(fp)),
          invoke_(&invoke_function_pointer_) {}

    // Construct from any non-`function_ref` callable invocable as
    // `R(Args...)`. The callable is referenced — not copied — so the caller
    // must keep it alive.
    template<class Fn,
             class = std::enable_if_t<
                 !std::is_same_v<std::remove_cvref_t<Fn>, function_ref> &&
                 std::is_invocable_r_v<R, Fn&, Args...>>>
    function_ref(Fn&& fn) noexcept
        : obj_(static_cast<const void*>(std::addressof(fn))),
          invoke_(&invoke_callable_<std::remove_reference_t<Fn>>) {}

    // Trivially copyable — implicit copy/move is correct.
    constexpr function_ref(const function_ref&) noexcept            = default;
    constexpr function_ref& operator=(const function_ref&) noexcept = default;

    constexpr function_ref& operator=(std::nullptr_t) noexcept {
        obj_    = nullptr;
        invoke_ = nullptr;
        return *this;
    }

    template<class Fn,
             class = std::enable_if_t<
                 !std::is_same_v<std::remove_cvref_t<Fn>, function_ref> &&
                 std::is_invocable_r_v<R, Fn&, Args...>>>
    function_ref& operator=(Fn&& fn) noexcept {
        obj_    = static_cast<const void*>(std::addressof(fn));
        invoke_ = &invoke_callable_<std::remove_reference_t<Fn>>;
        return *this;
    }

    // True iff the function_ref points to something invocable.
    explicit constexpr operator bool() const noexcept { return invoke_ != nullptr; }

    R operator()(Args... args) const {
        // UB to call when disengaged — same contract as a moved-from
        // std::function. Asserting here would impose a runtime cost on every
        // call; callers are expected to guard with `if (fr)` when relevant.
        return invoke_(obj_, std::forward<Args>(args)...);
    }

    friend constexpr bool operator==(const function_ref& a, std::nullptr_t) noexcept {
        return a.invoke_ == nullptr;
    }
    friend constexpr bool operator==(std::nullptr_t, const function_ref& a) noexcept {
        return a.invoke_ == nullptr;
    }
    friend constexpr bool operator!=(const function_ref& a, std::nullptr_t) noexcept {
        return a.invoke_ != nullptr;
    }
    friend constexpr bool operator!=(std::nullptr_t, const function_ref& a) noexcept {
        return a.invoke_ != nullptr;
    }

private:
    using Invoker = R (*)(const void*, Args...);

    template<class Fn>
    static R invoke_callable_(const void* obj, Args... args) {
        // const_cast because the stored pointer is `const void*` for a
        // strictly read-only handle representation, but the underlying
        // callable might be a mutable lambda or non-const operator().
        auto* p = const_cast<Fn*>(static_cast<const Fn*>(obj));
        return std::invoke(*p, std::forward<Args>(args)...);
    }

    static R invoke_function_pointer_(const void* obj, Args... args) {
        auto fp = reinterpret_cast<R (*)(Args...)>(const_cast<void*>(obj));
        return fp(std::forward<Args>(args)...);
    }

    const void* obj_    = nullptr;
    Invoker     invoke_ = nullptr;
};

// Deduction guide: enables `aria::function_ref f = some_lambda;` for the
// common case of a lambda whose call signature is a unique `R(Args...)`.
template<class R, class... Args>
function_ref(R (*)(Args...)) -> function_ref<R(Args...)>;

}  // namespace aria
