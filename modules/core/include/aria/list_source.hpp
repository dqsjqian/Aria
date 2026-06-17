#pragma once

// list_source.hpp — concept describing the "list-like source" surface.
//
// Adapter layer (Qt6 / AppKit / UIKit list bridges) wants to consume any
// of the observable list types uniformly:
//
//   * `aria::ObservableList<T>`
//   * `aria::FilteredList<T>`
//   * `aria::SortedList<T>`
//   * `aria::MappedList<S, T>`        (the element type is T = Target)
//   * `aria::DistinctList<T, Key>`
//   * `aria::PagedList<T>`
//   * `aria::GroupedList<T, Key>`     (the element type is T = Group<T, Key>)
//
// They all ship the same observation vocabulary (`ListChange<T>` events
// via `.observe(...)`) — supplied uniformly by `detail::ListSignalMixin`
// — plus a small read surface (`size()`, `at(i)`, `snapshot()`). Rather
// than introducing an abstract `IDerivedList<T>` base class — which
// would require either virtual dispatch or a type-erased wrapper — we
// express the requirement as a concept and let adapter templates accept
// the source by reference.
//
// Rationale (no-premature-abstraction principle):
//   * No new abstraction surface is introduced until a real demand
//     pulls one. The concept is the *minimum* contract every adapter
//     bridge needs.
//   * The four list types remain unchanged — they already satisfy the
//     concept by construction.
//   * Test code can supply tiny fakes that satisfy the concept without
//     pulling in `ObservableList` machinery.

#include "aria/observable_list.hpp"
#include "aria/subscription.hpp"

#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include <vector>

namespace aria {

/// `ListSource<L>` is satisfied by any type `L` whose elements `T` can
/// be discovered as `L::value_type` *or* `typename L::element_type`,
/// and which provides:
///
///   * `Subscription observe(std::function<void(const ListChange<T>&)>)`
///   * `std::size_t size() const`
///   * `std::shared_ptr<T> at(std::size_t) const`
///   * `std::vector<std::shared_ptr<T>> snapshot() const`
///
/// Use `list_source_value_t<L>` to recover the element type.

namespace detail {

template<typename L>
struct list_source_value_impl {
    template<typename U>
    static auto probe(int) -> typename U::value_type;
    template<typename U>
    static auto probe(...) -> typename U::element_type;
    using type = decltype(probe<L>(0));
};

}  // namespace detail

/// Element type for a list source. Resolves `value_type` (preferred)
/// then `element_type`. Both `ObservableList<T>` and the derived list
/// types expose one or the other via the `Signal` parametrisation, but
/// for the adapter contract we just need a stable name.
template<typename L>
using list_source_value_t = typename detail::list_source_value_impl<
    std::remove_cvref_t<L>>::type;

template<typename L, typename T>
concept ListSourceOf = requires(L& l,
                                std::function<void(const ListChange<T>&)> fn,
                                std::size_t i) {
    { l.observe(std::move(fn)) } -> std::same_as<Subscription>;
    { l.size() } -> std::convertible_to<std::size_t>;
    { l.at(i) } -> std::convertible_to<std::shared_ptr<T>>;
    { l.snapshot() } -> std::convertible_to<std::vector<std::shared_ptr<T>>>;
};

template<typename L>
concept ListSource = requires {
    typename list_source_value_t<L>;
} && ListSourceOf<L, list_source_value_t<L>>;

}  // namespace aria
