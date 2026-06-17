#include "aria/binding/view_model.hpp"
#include "aria/callback_boundary.hpp"

#include <utility>
#include <vector>

namespace aria::binding {

struct ViewModel::Impl {
    SubscriptionBag bag;
    Property<bool> is_active{false};
    std::vector<std::shared_ptr<ViewModel>> children;
    std::vector<std::function<void()>> on_destroy_hooks;
};

ViewModel::ViewModel() : impl_(std::make_unique<Impl>()) {}

ViewModel::~ViewModel() {
    while (!impl_->on_destroy_hooks.empty()) {
        auto hook = std::move(impl_->on_destroy_hooks.back());
        impl_->on_destroy_hooks.pop_back();
        try {
            if (hook) hook();
        } catch (...) {
            // Funnel hook failures through the unified callback-boundary
            // sink so they show up in diagnostics. Earlier revisions
            // silently swallowed every exception, which made debugging
            // a misbehaving destroy hook impossible. The destructor
            // itself stays noexcept by routing through the (noexcept)
            // sink rather than re-throwing.
            ::aria::report_callback_failure(
                std::string_view{"vm.destroy_hook"},
                std::current_exception());
        }
    }
}

Property<bool>& ViewModel::is_active()       { return impl_->is_active; }
const Property<bool>& ViewModel::is_active() const { return impl_->is_active; }

SubscriptionBag& ViewModel::bag() { return impl_->bag; }

void ViewModel::activate() {
    if (impl_->is_active.get()) return;

    // Run the user-defined `on_activate()` BEFORE flipping the
    // `is_active` flag, then propagate to children, and only then flip
    // the flag in a single batched commit. This way:
    //   * Observers of `is_active` see `true` only after the VM is
    //     fully ready (not while `on_activate` is mid-flight).
    //   * If `on_activate()` throws, we never observe the flag flipped.
    //   * The state propagation across parent + every child is atomic
    //     from the reactive graph's point of view (one flush only).
    on_activate();
    ::aria::reactive::batch([&]{
        impl_->is_active.set(true);
        for (auto& c : impl_->children) c->activate();
    });
}

void ViewModel::deactivate() {
    if (!impl_->is_active.get()) return;

    // Symmetric to `activate()`: deactivate children first, run the
    // user hook with the flag still `true` (so the hook sees a live
    // VM), then flip to `false` in a single batch.
    ::aria::reactive::batch([&]{
        for (auto& c : impl_->children) c->deactivate();
    });
    on_deactivate();
    impl_->is_active.set(false);
}

void ViewModel::add_child(std::shared_ptr<ViewModel> child) {
    impl_->children.push_back(std::move(child));
}

void ViewModel::track(Subscription sub) {
    impl_->bag.add(std::move(sub));
}

void ViewModel::add_destroy_hook(std::function<void()> hook) {
    if (hook) impl_->on_destroy_hooks.push_back(std::move(hook));
}

}  // namespace aria::binding
