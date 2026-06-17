#pragma once

// ============================================================================
//  aria/concepts.hpp
// ----------------------------------------------------------------------------
//  Central catalogue of public-facing concepts used across the framework.
//
//  Per `docs/api-style.md` S-30, every templated public entry point should
//  prefer a named concept here over inline `requires` clauses. That way
//  IDEs and compilers can both surface a one-line "constraint not
//  satisfied" diagnostic instead of a multi-screen SFINAE explosion.
//
//  Concepts live in `aria::` so users never have to qualify them with
//  `aria::reactive::` or any other implementation namespace.
// ============================================================================

#include <concepts>
#include <functional>
#include <type_traits>

namespace aria {

// ---------------------------------------------------------------------------
//  Equality / property-value primitives
// ---------------------------------------------------------------------------

/// Type that supports `==` and `!=` (required for change detection).
template<typename T>
concept EqualityComparable = requires(const T& a, const T& b) {
    { a == b } -> std::convertible_to<bool>;
    { a != b } -> std::convertible_to<bool>;
};

/// Anything that can be invoked with `Args...` and returns
/// convertible-to-Ret. Convenience over `std::invocable + invoke_result`.
template<typename F, typename Ret, typename... Args>
concept InvocableR = std::invocable<F, Args...>
                  && std::convertible_to<std::invoke_result_t<F, Args...>, Ret>;

/// A read-only observable: has `.get()` and exposes its `value_type`.
/// The on_changed surface is intentionally not part of the concept here
/// because `Property<T>` and `Computed<T>` deliver it via type-erased
/// `Subscription on_changed(std::function<void(const T&)>)` which is
/// hard to express as a concept without inducing the very SFINAE noise
/// we are trying to remove. Adapter code that genuinely needs both
/// surfaces should constrain on `Observable` here AND check
/// `requires(t) { t.on_changed(...); }` locally.
template<typename T>
concept Observable = requires(T t) {
    typename T::value_type;
    { t.get() } -> std::convertible_to<typename T::value_type>;
};

/// Type usable as a Property value:
///   - copyable (we copy old value to pass to observers)
///   - equality comparable (so we can skip notifications on no-op writes)
///
/// This is the canonical constraint for `Property<T>`, `Computed<T>`
/// and anywhere a "boxed reactive cell" is required.
template<typename T>
concept PropertyValue = std::copyable<T> && EqualityComparable<T>;

// ---------------------------------------------------------------------------
//  Reactive graph participation
// ---------------------------------------------------------------------------
//
//  `aria::ReactiveNode<T>` (constraining types that play in the dependency
//  graph, i.e. inherit `aria::reactive::Node`) is defined alongside its
//  required complete type in `<aria/reactive/node.hpp>`. Including it
//  here would force every `<aria/concepts.hpp>` consumer to drag the
//  whole reactive subsystem; users that need `ReactiveNode` will already
//  be including `<aria/property.hpp>` (or similar) which transitively
//  pulls it in, so the concept is still surfaced from `aria::`.

}  // namespace aria
