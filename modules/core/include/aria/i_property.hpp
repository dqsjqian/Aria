#pragma once

// IProperty — type-erased, ABI-friendly view over a Property<T>.
//
// `aria::Property<T>` inherits from this interface and
// implements all four virtuals.
//
// Motivation:
//   `Property<T>` is a header-only template, so its symbols live in the
//   consumer's translation unit. That is fine for an in-process API,
//   but plug-ins / RPC / live binding scenarios need a stable,
//   non-template surface. IProperty is that surface.
//
// Design:
//   * One virtual call per get / set / subscribe.
//   * `std::any` for the value payload (so the dynamic library and
//     consumer don't have to agree on `T`'s mangled name).
//   * No throwing accessor: a wrong-typed `set_any` returns false and
//     leaves the property untouched, matching how dynamic binding
//     systems handle type mismatches.
//
// Threading:
//   The same contract as `Property<T>::set/get` — all four virtuals
//   must be invoked on the graph's owning thread (assertions fire
//   in debug builds otherwise). Cross-thread plug-ins must marshal
//   through a Dispatcher, exactly like typed callers.
//
// Cost vs template direct call (microbench in modules/core/bench):
//   ~5-7 ns extra per get/set on Apple Silicon for trivially-copyable
//   payloads (int, double — fit in std::any's SBO). Larger payload
//   types pay for an additional heap allocation when std::any spills
//   out of its SBO, so the gap widens. Either way, template-direct
//   should still be preferred in hot paths inside the host module.
//   See `benchmark/bench_iproperty.cpp` for the full numbers.

#include "aria/subscription.hpp"

#include <any>
#include <functional>
#include <typeinfo>

namespace aria {

/// Type-erased Property surface. Stable, non-template, suitable for
/// crossing a shared-library boundary.
///
/// Implementations:
///   * `aria::Property<T>` — the canonical implementation.
///   * Test doubles can implement this directly to drive
///     binding-engine code without instantiating a real Property.
class IProperty {
public:
    virtual ~IProperty() noexcept = default;

    /// Read the current value as a `std::any`. The runtime type is
    /// guaranteed to equal `type()`.
    [[nodiscard]] virtual std::any get_any() const = 0;

    /// Try to set the property's value from a `std::any`. Returns
    /// true on success, false when the runtime types don't match
    /// (in which case the property is left unchanged).
    [[nodiscard]] virtual bool set_any(const std::any& value) = 0;

    /// Subscribe to value changes. The callback receives the new
    /// value as `std::any` of the same runtime type as `type()`.
    [[nodiscard]] virtual Subscription subscribe_any(
        std::function<void(const std::any&)> on_changed) = 0;

    /// The runtime type of the wrapped value. Useful for callers
    /// that want to gate their `std::any_cast` on a name comparison
    /// before paying the cost of the cast.
    [[nodiscard]] virtual const std::type_info& type() const noexcept = 0;

protected:
    IProperty() noexcept = default;
    IProperty(const IProperty&) = delete;
    IProperty& operator=(const IProperty&) = delete;
    IProperty(IProperty&&)                 = delete;
    IProperty& operator=(IProperty&&)      = delete;
};

}  // namespace aria
