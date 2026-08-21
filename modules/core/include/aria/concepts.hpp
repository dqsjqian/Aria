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
#include <optional>
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
//  Read-only reactive sources
// ---------------------------------------------------------------------------

/// A reactive cell that can be **read** and **observed**, but not written:
/// exactly the surface a one-way (VM→View) binding needs.
///
/// Both `Property<T>` and `Computed<T>` satisfy it — they expose the same
/// `value_type` / `get()` / `on_changed(std::function<void(const T&)>)`
/// triple. `BindingEngine`'s one-way binders constrain on this instead of
/// the concrete `Property<T>`, which is what makes a derived value
/// (`Computed`) bindable without a hand-written `on_changed` plus a
/// caller-owned subscription store.
///
/// Deliberately **not** satisfied by anything write-only, and deliberately
/// *not* extended with `set()`: two-way binders keep taking `Property<T>&`
/// so binding a computed value two-way stays a compile error rather than a
/// silently dropped write-back.
///
/// Unlike `Observable` above, this concept does include the `on_changed`
/// surface. That is possible because both implementations type-erase the
/// callback to `std::function`, so the check is a single well-formed call
/// expression rather than an open-ended invocable probe.
template<typename S>
concept ReadOnlyReactive =
    requires(S& s) {
        typename S::value_type;
        { s.get() } -> std::convertible_to<typename S::value_type>;
    } &&
    requires(S& s, std::function<void(const typename S::value_type&)> fn) {
        s.on_changed(std::move(fn));
    };

/// `ReadOnlyReactive` pinned to a specific value type — the constraint for
/// the typed scalar binders (`bind_text_oneway` wants `value_type` to be
/// exactly `std::string`, `bind_visible` wants `bool`, ...).
template<typename S, typename T>
concept ReadOnlyReactiveOf = ReadOnlyReactive<S>
                          && std::same_as<typename S::value_type, T>;

namespace detail {

template<typename T> struct is_optional                    : std::false_type {};
template<typename U> struct is_optional<std::optional<U>>   : std::true_type  {};

template<typename T>
inline constexpr bool is_optional_v = is_optional<T>::value;

}  // namespace detail

/// A `ReadOnlyReactive` whose `value_type` is some `std::optional<U>` —
/// the shape of `AsyncCommand::last_result`, and the constraint for
/// `BindingEngine::bind_optional_text`.
template<typename S>
concept ReadOnlyReactiveOptional = ReadOnlyReactive<S>
                                && detail::is_optional_v<typename S::value_type>;

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
