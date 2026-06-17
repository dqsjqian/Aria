#pragma once

// list_signal_mixin.hpp — CRTP mixin that unifies the
//
//     Subscription   observe(std::function<void(const ListChange<E>&)>)
//     Subscription   on_any_change(std::function<void()>)
//     std::size_t    observer_count() const noexcept
//
// triple shared by `ObservableList<T>` and every `*List` derived view
// (filtered / sorted / mapped / distinct / paged / grouped). Before
// this mixin existed, the three functions were copy-pasted across
// seven files — every new derived list had to re-implement the exact
// same forwarding boilerplate, and a single API tweak (e.g. adding
// `[[nodiscard]]`, renaming the wrapper, or changing the noexcept
// contract) had to be applied seven times in lockstep.
//
// Design notes:
//
//  1. **Pure CRTP, zero runtime cost.** The mixin reaches into the
//     derived class via `static_cast<const Derived*>(this)->signal_`
//     to avoid taking a virtual call. The compiler inlines every
//     member straight through to the underlying TypedSignal.
//
//  2. **No vtable, no ABI surface.** The mixin is header-only and
//     stateless; deriving from it adds nothing to the object layout
//     and cannot break dllexport/dllimport contracts.
//
//  3. **Element type is explicit.** The element type `E` is a
//     template parameter of the mixin (not deduced from the derived
//     class) so derived lists whose change-element differs from the
//     stored element — for example `MappedList<S, Target>` (E ==
//     Target) and `GroupedList<T, Key>` (E == Group<T, Key>) — can
//     declare exactly the surface they want.
//
//  4. **Contract on Derived.** Derived must expose a member
//     `std::shared_ptr<detail::TypedSignal<ListChange<E>>> signal_`.
//     We use a `protected` accessor so the mixin can find it without
//     forcing each Derived to declare a friendship.
//
// Usage:
//
//     template<typename T>
//     class FilteredList
//         : public detail::ListSignalMixin<FilteredList<T>, T> {
//         friend detail::ListSignalMixin<FilteredList<T>, T>;
//         // ...
//         std::shared_ptr<detail::TypedSignal<ListChange<T>>> signal_;
//     };

#include "aria/detail/typed_signal.hpp"
#include "aria/subscription.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <utility>

namespace aria {

template<typename T>
struct ListChange;

namespace detail {

template<typename Derived, typename E>
class ListSignalMixin {
public:
    /// Subscribe to every structural / item change reported by the
    /// list. Each fired event carries a fully-populated `ListChange<E>`
    /// (kind, index, item pointer, optional from_index for Move).
    [[nodiscard]] Subscription
    observe(std::function<void(const ListChange<E>&)> fn) {
        return signal_ref_().connect(std::move(fn));
    }

    /// Coarse-grained observer that fires once per change with no
    /// payload. Convenient for "rebuild whole view" sinks (UI tables
    /// that diff their own snapshot, debug counters, dirty flags, …).
    [[nodiscard]] Subscription
    on_any_change(std::function<void()> fn) {
        return signal_ref_().connect(
            [f = std::move(fn)](const ListChange<E>&) { f(); });
    }

    /// Number of currently-connected observers (sum of `observe(...)`
    /// and `on_any_change(...)` slots). Used by tests and by adapter
    /// teardown logic to assert clean detach. Cheap (atomic load on
    /// the underlying signal).
    [[nodiscard]] std::size_t observer_count() const noexcept {
        return signal_ref_().slot_count();
    }

private:
    using SignalT = TypedSignal<ListChange<E>>;

    SignalT& signal_ref_() noexcept {
        // The Derived must expose `std::shared_ptr<SignalT> signal_`
        // (declared at any access level — friendship is granted by
        // Derived to this mixin specialisation).
        return *static_cast<Derived*>(this)->signal_;
    }
    const SignalT& signal_ref_() const noexcept {
        return *static_cast<const Derived*>(this)->signal_;
    }
};

}  // namespace detail
}  // namespace aria
