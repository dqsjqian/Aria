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
// * Member pointers are rejected; a null free-function pointer produces a
//   disengaged view. Function pointers are stored by value.
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

    // The exact-signature overload also resolves overloaded function names.
    function_ref(R (*fp)(Args...)) noexcept
        : target_{.function = reinterpret_cast<ErasedFunction>(fp)},
          invoke_(fp ? &invoke_function_pointer_<decltype(fp)> : nullptr) {}

    // Store function pointers by value, including implicit function decay
    // and noexcept functions. The generic object path must never retain a
    // pointer to the temporary pointer object at a construction/assignment.
    template<class Fp,
             std::enable_if_t<std::is_pointer_v<Fp> &&
                              std::is_function_v<std::remove_pointer_t<Fp>> &&
                              std::is_invocable_r_v<R, Fp, Args...>, int> = 0>
    function_ref(Fp fp) noexcept
        : target_{.function = reinterpret_cast<ErasedFunction>(fp)},
          invoke_(fp ? &invoke_function_pointer_<Fp> : nullptr) {}

    // Construct from any non-`function_ref` callable invocable as
    // `R(Args...)`. The callable is referenced — not copied — so the caller
    // must keep it alive.
    template<class Fn,
             class = std::enable_if_t<
                 !std::is_same_v<std::remove_cvref_t<Fn>, function_ref> &&
                 !std::is_pointer_v<std::remove_cvref_t<Fn>> &&
                 !std::is_function_v<std::remove_reference_t<Fn>> &&
                 !std::is_member_pointer_v<std::remove_cvref_t<Fn>> &&
                 std::is_invocable_r_v<R, Fn&, Args...>>>
    function_ref(Fn&& fn) noexcept
        : target_{.object = static_cast<const void*>(std::addressof(fn))},
          invoke_(&invoke_callable_<std::remove_reference_t<Fn>>) {}

    // Trivially copyable — implicit copy/move is correct.
    constexpr function_ref(const function_ref&) noexcept            = default;
    constexpr function_ref& operator=(const function_ref&) noexcept = default;

    constexpr function_ref& operator=(std::nullptr_t) noexcept {
        target_.object = nullptr;
        invoke_ = nullptr;
        return *this;
    }

    function_ref& operator=(R (*fp)(Args...)) noexcept {
        target_.function = reinterpret_cast<ErasedFunction>(fp);
        invoke_ = fp ? &invoke_function_pointer_<decltype(fp)> : nullptr;
        return *this;
    }

    template<class Fp,
             std::enable_if_t<std::is_pointer_v<Fp> &&
                              std::is_function_v<std::remove_pointer_t<Fp>> &&
                              std::is_invocable_r_v<R, Fp, Args...>, int> = 0>
    function_ref& operator=(Fp fp) noexcept {
        target_.function = reinterpret_cast<ErasedFunction>(fp);
        invoke_ = fp ? &invoke_function_pointer_<Fp> : nullptr;
        return *this;
    }

    template<class Fn,
             class = std::enable_if_t<
                 !std::is_same_v<std::remove_cvref_t<Fn>, function_ref> &&
                 !std::is_pointer_v<std::remove_cvref_t<Fn>> &&
                 !std::is_function_v<std::remove_reference_t<Fn>> &&
                 !std::is_member_pointer_v<std::remove_cvref_t<Fn>> &&
                 std::is_invocable_r_v<R, Fn&, Args...>>>
    function_ref& operator=(Fn&& fn) noexcept {
        target_.object = static_cast<const void*>(std::addressof(fn));
        invoke_ = &invoke_callable_<std::remove_reference_t<Fn>>;
        return *this;
    }

    // True iff the function_ref points to something invocable.
    explicit constexpr operator bool() const noexcept { return invoke_ != nullptr; }

    R operator()(Args... args) const {
        // UB to call when disengaged. Asserting here would impose a cost on every
        // call; callers are expected to guard with `if (fr)` when relevant.
        return invoke_(target_, std::forward<Args>(args)...);
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
    using ErasedFunction = void (*)();
    union Target {
        const void* object = nullptr;
        ErasedFunction function;
    };
    using Invoker = R (*)(Target, Args...);

    template<class Fn>
    static R invoke_callable_(Target target, Args... args) {
        // Preserve const targets while allowing mutable non-const callables.
        auto* p = const_cast<Fn*>(static_cast<const Fn*>(target.object));
        if constexpr (std::is_void_v<R>) {
            std::invoke(*p, std::forward<Args>(args)...);
        } else {
            return std::invoke(*p, std::forward<Args>(args)...);
        }
    }

    template<class Fp>
    static R invoke_function_pointer_(Target target, Args... args) {
        // Function-pointer conversions are standard reversible conversions;
        // no assumption that an object pointer can represent a function.
        auto fp = reinterpret_cast<Fp>(target.function);
        if constexpr (std::is_void_v<R>) {
            std::invoke(fp, std::forward<Args>(args)...);
        } else {
            return std::invoke(fp, std::forward<Args>(args)...);
        }
    }

    Target target_{};
    Invoker invoke_ = nullptr;
};

// Deduction guide: enables `aria::function_ref f = some_lambda;` for the
// common case of a lambda whose call signature is a unique `R(Args...)`.
template<class R, class... Args>
function_ref(R (*)(Args...)) -> function_ref<R(Args...)>;

}  // namespace aria
