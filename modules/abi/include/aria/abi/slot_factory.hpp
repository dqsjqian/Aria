#pragma once

// ============================================================================
//  abi/slot_factory.hpp
// ----------------------------------------------------------------------------
//  Header-only factories that turn an arbitrary callable into a
//  type-erased `aria::abi::SlotErased`.
//
//  Why factories live here, not in each consumer:
//    Before this header existed, four independent translation units
//    (detail/typed_signal.hpp + qt6/appkit/uikit adapters) each spelled
//    out the same heap-allocate-state / invoker-trampoline / destroyer
//    pattern. That is bug-prone (subtly inconsistent exception policies
//    drifted across the four copies) and has no business being repeated.
//    Concentrating it here gives one canonical, exception-safe spelling.
//
//  Two flavors are offered:
//
//    make_slot_erased<Fn>(fn)
//        The callable accepts a raw `void* args` (the caller is
//        responsible for casting). Used by adapters that already build
//        a typed args bag and forward the pointer.
//
//    make_slot_for<Bag, Fn>(fn)
//        The callable accepts `const Bag&`. The cast from `void*` is
//        performed inside the trampoline. This is the form ~all real
//        callers actually want.
//
//  Exception policy:
//    The trampoline catches all exceptions from the user callable.
//    Slot invocation must be `noexcept` at the ABI boundary, so any
//    user exception is contained locally rather than being allowed to
//    cross the trampoline (which would terminate the process). When a
//    slot-invoke failure hook has been installed (typically by `core`
//    via `aria::abi::set_slot_invoke_failure_hook`), the captured
//    exception is reported through the hook before the trampoline
//    returns; otherwise it is reported through the unified callback-failure
//    boundary, whose default sink writes to stderr.
//
//    The hook itself is **not** allowed to throw — it is invoked from a
//    noexcept boundary. A throwing hook falls back to the unified reporter.
//
//  Allocation:
//    State is stored on the heap (one allocation) because we cannot
//    place arbitrary callables into the small `void*` slot. The state
//    is owned by the SlotErased and freed via the destroyer trampoline.
//    A `std::unique_ptr` holds the state until ownership transfers,
//    even though the SlotErased ctor itself is `noexcept` -- this keeps
//    the factory exception-safe in the face of future ABI changes.
// ============================================================================

#include "aria/abi/slot.hpp"

#include <atomic>
#include <exception>
#include <memory>
#include <type_traits>
#include <utility>

namespace aria::abi {

/// Hook invoked when the slot trampoline catches an exception escaping
/// the user callable. The implementation is provided by upper layers
/// (`core`'s `callback_boundary`) at process startup. Installing
/// `nullptr` uses the unified callback-failure reporter. Hooks should avoid
/// throwing; exceptions from a hook fall back to the unified reporter.
using SlotInvokeFailureHook = void (*)(std::exception_ptr);

namespace detail {

// Storage lives in libaria_abi (single TU) so every SHARED consumer
// reaches the same physical slot. See callback_boundary.cpp.
ARIA_ABI_API std::atomic<SlotInvokeFailureHook>& slot_invoke_failure_hook() noexcept;

ARIA_ABI_API void report_slot_invoke_failure_(std::exception_ptr exception) noexcept;

}  // namespace detail

/// Install (or replace) the slot-invoke failure hook. Returns the
/// previously installed hook (`nullptr` if none).
inline SlotInvokeFailureHook
set_slot_invoke_failure_hook(SlotInvokeFailureHook hook) noexcept {
    return detail::slot_invoke_failure_hook().exchange(hook, std::memory_order_acq_rel);
}

namespace detail {

// Trampoline for the void*-args flavor: F is invoked with the raw args.
template <class F>
inline constexpr SlotErased::Invoker raw_invoker_v =
    [](void* state, void* args) noexcept {
        try {
            (*static_cast<F*>(state))(args);
        } catch (...) {
            detail::report_slot_invoke_failure_(std::current_exception());
        }
    };

// Trampoline for the typed-bag flavor: F is invoked with `const Bag&`.
// `args` must point to a valid Bag, including when Bag is an empty type.
// Signals with no payload can instead use the raw void* flavor.
template <class F, class Bag>
inline constexpr SlotErased::Invoker bag_invoker_v =
    [](void* state, void* args) noexcept {
        try {
            (*static_cast<F*>(state))(*static_cast<const Bag*>(args));
        } catch (...) {
            detail::report_slot_invoke_failure_(std::current_exception());
        }
    };

// Common destroyer.
template <class F>
inline constexpr SlotErased::Destroyer destroyer_v =
    [](void* state) noexcept { delete static_cast<F*>(state); };

}  // namespace detail

/// Build a SlotErased from a callable that takes a raw `void* args`.
/// The callable is heap-allocated and owned by the returned slot.
template <class Fn>
[[nodiscard]] SlotErased make_slot_erased(Fn&& fn) {
    using F = std::decay_t<Fn>;
    static_assert(std::is_invocable_v<F&, void*>,
                  "make_slot_erased: Fn must be callable with void*");

    // Hold via unique_ptr until ownership transfers into SlotErased.
    auto state = std::make_unique<F>(std::forward<Fn>(fn));
    SlotErased slot{detail::raw_invoker_v<F>,
                    detail::destroyer_v<F>,
                    state.get()};
    (void)state.release();
    return slot;
}

/// Build a SlotErased from a callable that takes `const Bag&`. The
/// cast from the wire `void*` happens inside the trampoline.
template <class Bag, class Fn>
[[nodiscard]] SlotErased make_slot_for(Fn&& fn) {
    using F = std::decay_t<Fn>;
    static_assert(std::is_invocable_v<F&, const Bag&>,
                  "make_slot_for: Fn must be callable with const Bag&");

    auto state = std::make_unique<F>(std::forward<Fn>(fn));
    SlotErased slot{detail::bag_invoker_v<F, Bag>,
                    detail::destroyer_v<F>,
                    state.get()};
    (void)state.release();
    return slot;
}

}  // namespace aria::abi
