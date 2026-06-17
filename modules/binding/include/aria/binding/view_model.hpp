#pragma once

#include "aria/abi/export.hpp"
#include "aria/property.hpp"
#include "aria/subscription.hpp"

#include <functional>
#include <memory>
#include <vector>

namespace aria::binding {

/// Base class for view models. Provides lifecycle, child management, and a
/// subscription bag that auto-cleans up on destruction.
///
/// For structured concurrency (auto-cancel of in-flight coroutines on VM
/// destruction), use the `ViewModelScope` helper from
/// `aria/binding/view_model_scope.hpp`.
///
/// The dtor / activate / deactivate / add_child / add_destroy_hook impls
/// live out-of-line in view_model.cpp so the vtable is emitted exactly once
/// inside the binding library (matters on Windows DLL builds, reduces code
/// size, and speeds compilation of every TU that only uses the interface).
///
/// MSVC C4251: enable_shared_from_this<ViewModel> contains a weak_ptr whose
/// layout is guaranteed by the C++ Standard Library ABI. All other STL
/// members have been moved into the Impl. Suppression is safe.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4251)
#endif
class ARIA_BINDING_API ViewModel : public std::enable_shared_from_this<ViewModel> {
public:
    ViewModel();
    virtual ~ViewModel();

    /// Activation state (Property<bool>). Returns a reference so consumers
    /// can call `.get()`, `.set(v)`, `.on_changed(...)` etc. as before.
    /// The backing store lives in Impl, keeping the DLL interface clean.
    [[nodiscard]] Property<bool>& is_active();
    [[nodiscard]] const Property<bool>& is_active() const;

    virtual void on_activate() {}
    virtual void on_deactivate() {}

    void activate();
    void deactivate();

    void add_child(std::shared_ptr<ViewModel> child);

    /// Add a subscription that will be released on destruction.
    void track(Subscription sub);

    void add_destroy_hook(std::function<void()> hook);

protected:
    /// Access the subscription bag. Subclass VMs use this to attach
    /// subscriptions that auto-clean on deactivation/destruction.
    [[nodiscard]] SubscriptionBag& bag();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

/// `IViewModel` is an alias for `ViewModel`, provided for users coming
/// from WPF / Avalonia / MAUI where the `IViewModel` spelling is
/// canonical. Note that despite the `I` prefix this is NOT a pure
/// virtual interface — `ViewModel` is a concrete base class with
/// state (`is_active`, subscription bag, child list, destroy hooks)
/// and default implementations for `on_activate` / `on_deactivate`.
/// The `IView` / `IViewAdapter` types in this library really are
/// pure interfaces and keep the `I` prefix; `ViewModel` does not,
/// because hiding its stateful nature behind an `I` would be
/// misleading. This alias exists purely to match external naming
/// conventions without forcing the rename on internals.
using IViewModel = ViewModel;

}  // namespace aria::binding
