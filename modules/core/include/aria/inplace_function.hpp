#pragma once

// aria::inplace_function<R(Args...), Capacity, Align> — small-object owning
// type-erased callable.
//
// Inspired by `std::inplace_function` (P0228) and the various battle-tested
// implementations shipped by Boost / EA / fixed_callable. Holds the callable
// in an internal aligned buffer of `Capacity` bytes; if the callable cannot
// fit (size or alignment), the program is rejected at compile time. There is
// **no** heap fallback — by design.
//
// Design contract
// ---------------
// 1. **Owns its callable.** Move/copy-constructs / destroys the underlying
//    object exactly when you would expect from `std::function`.
// 2. **Zero heap allocation.** A storage overflow is a static_assert, not a
//    runtime malloc.
// 3. **Two pointers + buffer.** Layout is `(invoker_ptr, manager_ptr,
//    aligned_buffer)`. `invoker_ptr` calls the wrapped callable; the
//    `manager_ptr` is a single function pointer that handles destroy /
//    move via a tag dispatch (one indirection rather than three).
// 4. **Trivially small lambdas optimised.** When the captured callable is
//    trivially copyable & trivially destructible, the manager is a tiny
//    memcpy; when not, the manager forwards to typed move/destroy helpers.
// 5. **Empty-state safe.** `operator bool()` reports engagement; calling an
//    empty `inplace_function` throws `aria::bad_inplace_function_call`,
//    which derives from `std::bad_function_call`.
//
// Why not just std::function
// --------------------------
// `std::function` permits, but does not require, small-buffer optimisation.
// Many standard libraries kick into heap allocation when the captured state
// exceeds an undocumented threshold (libc++: 24 bytes on 64-bit). For hot
// derived-list predicates, comparators and mappers we want a hard guarantee:
// no allocation, period. `inplace_function` is that guarantee, expressed in
// the type system.
//
// Capacity guidance
// -----------------
// * 32 bytes is the project default — fits a `[a, b, c, d](){...}` capture
//   plus four 8-byte references on 64-bit, with room for a small struct.
// * If a binding site needs more, bump the capacity for that site. The
//   `static_assert` will scream loudly when the constraint is violated.

#include "aria/function_ref.hpp"

#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace aria {

class bad_inplace_function_call : public std::bad_function_call {
public:
    [[nodiscard]] const char* what() const noexcept override {
        return "aria::inplace_function: invoking an empty wrapper";
    }
};

namespace detail::inplace {

// Manager opcodes — a single function-pointer per concrete callable handles
// move, copy, and destruction. Using a single manager pointer (rather than
// one for each operation) keeps the `inplace_function` footprint tight.
//
// `Copy` is only supported for callables that are themselves
// `CopyConstructible`; the inplace_function copy constructor / copy
// assignment are SFINAE-disabled when the held callable is move-only, so
// `Op::Copy` is never reachable in that case. We still always synthesise
// the manager — invoking it with `Copy` for a non-copyable callable would
// be a static_assert at the manager template instantiation site.
enum class Op : unsigned char {
    Destroy,
    MoveConstruct,
    CopyConstruct,
};

template<class Fn>
void manager_for(Op op, void* self, void* other) noexcept {
    auto* dst = static_cast<Fn*>(self);
    switch (op) {
        case Op::Destroy:
            dst->~Fn();
            return;
        case Op::MoveConstruct: {
            auto* src = static_cast<Fn*>(other);
            ::new (dst) Fn(std::move(*src));
            return;
        }
        case Op::CopyConstruct: {
            if constexpr (std::is_copy_constructible_v<Fn>) {
                const auto* src = static_cast<const Fn*>(other);
                ::new (dst) Fn(*src);
            } else {
                // Unreachable: the inplace_function copy constructor is
                // SFINAE-disabled when Fn is move-only. We still need the
                // case to keep the switch exhaustive.
            }
            return;
        }
    }
}

}  // namespace detail::inplace

template<class Sig,
         std::size_t Capacity  = 32,
         std::size_t Alignment = alignof(std::max_align_t)>
class inplace_function;

template<class R, class... Args, std::size_t Capacity, std::size_t Alignment>
class inplace_function<R(Args...), Capacity, Alignment> {
public:
    using result_type = R;

    constexpr inplace_function() noexcept = default;

    inplace_function(std::nullptr_t) noexcept {}

    template<class Fn,
             class Decayed = std::decay_t<Fn>,
             class = std::enable_if_t<
                 !std::is_same_v<Decayed, inplace_function> &&
                 std::is_invocable_r_v<R, Decayed&, Args...> &&
                 std::is_move_constructible_v<Decayed>>>
    inplace_function(Fn&& fn) {
        emplace_<Decayed>(std::forward<Fn>(fn));
    }

    inplace_function(const inplace_function& other) {
        copy_from_(other);
    }

    inplace_function& operator=(const inplace_function& other) {
        if (this != &other) {
            reset();
            copy_from_(other);
        }
        return *this;
    }

    inplace_function(inplace_function&& other) noexcept {
        move_from_(other);
    }

    inplace_function& operator=(inplace_function&& other) noexcept {
        if (this != &other) {
            reset();
            move_from_(other);
        }
        return *this;
    }

    inplace_function& operator=(std::nullptr_t) noexcept {
        reset();
        return *this;
    }

    template<class Fn,
             class Decayed = std::decay_t<Fn>,
             class = std::enable_if_t<
                 !std::is_same_v<Decayed, inplace_function> &&
                 std::is_invocable_r_v<R, Decayed&, Args...> &&
                 std::is_move_constructible_v<Decayed>>>
    inplace_function& operator=(Fn&& fn) {
        reset();
        emplace_<Decayed>(std::forward<Fn>(fn));
        return *this;
    }

    ~inplace_function() { reset(); }

    void reset() noexcept {
        if (manager_ != nullptr) {
            manager_(detail::inplace::Op::Destroy, storage_(), nullptr);
            manager_ = nullptr;
            invoker_ = nullptr;
        }
    }

    explicit operator bool() const noexcept { return invoker_ != nullptr; }

    R operator()(Args... args) const {
        if (invoker_ == nullptr) {
            throw bad_inplace_function_call{};
        }
        return invoker_(storage_(), std::forward<Args>(args)...);
    }

    // Implicit conversion to a non-owning view — `function_ref` always
    // remains valid for the lifetime of the `inplace_function`. The view
    // captures `*this`, not the raw invoker pointer; that lets it route
    // through `operator()` (which already knows how to dispatch the held
    // callable through the type-erased `invoker_`), avoiding the
    // signature mismatch between the internal
    // `R(*)(const void*, Args...)` invoker and the public
    // `R(*)(Args...)` view contract.
    operator function_ref<R(Args...)>() const noexcept {
        if (invoker_ == nullptr) return {};
        return function_ref<R(Args...)>{*this};
    }

    // Convenience: take a non-owning view explicitly. Equivalent to the
    // implicit conversion above; provided so callers can spell the
    // intent without resorting to a `static_cast<function_ref<...>>`.
    [[nodiscard]] function_ref<R(Args...)> ref() const noexcept {
        return static_cast<function_ref<R(Args...)>>(*this);
    }

    friend bool operator==(const inplace_function& a, std::nullptr_t) noexcept {
        return a.invoker_ == nullptr;
    }
    friend bool operator==(std::nullptr_t, const inplace_function& a) noexcept {
        return a.invoker_ == nullptr;
    }
    friend bool operator!=(const inplace_function& a, std::nullptr_t) noexcept {
        return a.invoker_ != nullptr;
    }
    friend bool operator!=(std::nullptr_t, const inplace_function& a) noexcept {
        return a.invoker_ != nullptr;
    }

private:
    using Invoker = R (*)(const void*, Args...);
    using ManagerFn =
        void (*)(detail::inplace::Op, void* /*self*/, void* /*other*/) noexcept;

    template<class Stored, class U>
    void emplace_(U&& fn) {
        static_assert(sizeof(Stored) <= Capacity,
            "aria::inplace_function: callable does not fit. "
            "Either reduce capture size or increase Capacity.");
        static_assert(alignof(Stored) <= Alignment,
            "aria::inplace_function: callable alignment exceeds Alignment. "
            "Increase the Alignment template parameter.");

        ::new (storage_()) Stored(std::forward<U>(fn));
        invoker_ = &invoke_<Stored>;
        manager_ = &detail::inplace::manager_for<Stored>;
    }

    template<class Fn>
    static R invoke_(const void* obj, Args... args) {
        // const_cast: the underlying callable may have a non-const operator()
        // (mutable lambdas). The storage is morally non-const; we only mark
        // it const for the function_ref interop.
        auto* p = const_cast<Fn*>(static_cast<const Fn*>(obj));
        return std::invoke(*p, std::forward<Args>(args)...);
    }

    void move_from_(inplace_function& other) noexcept {
        if (other.manager_ != nullptr) {
            other.manager_(detail::inplace::Op::MoveConstruct,
                           storage_(), other.storage_());
            invoker_ = other.invoker_;
            manager_ = other.manager_;
            other.reset();
        }
    }

    void copy_from_(const inplace_function& other) {
        if (other.manager_ != nullptr) {
            // The manager dispatches to a typed copy that performs a
            // placement-new copy construction of the held callable.
            // For move-only Fn the relevant `Op::Copy` arm is unreachable
            // (see manager_for); the inplace_function copy ctor itself
            // would fail to instantiate via SFINAE upstream. Here we are
            // already on the runtime copy path, so the held type is
            // necessarily CopyConstructible.
            other.manager_(detail::inplace::Op::CopyConstruct,
                           storage_(),
                           const_cast<void*>(other.storage_()));
            invoker_ = other.invoker_;
            manager_ = other.manager_;
        }
    }

    void* storage_() noexcept {
        return static_cast<void*>(&buffer_);
    }
    const void* storage_() const noexcept {
        return static_cast<const void*>(&buffer_);
    }

    alignas(Alignment) std::byte buffer_[Capacity]{};
    Invoker   invoker_ = nullptr;
    ManagerFn manager_ = nullptr;
};

}  // namespace aria
