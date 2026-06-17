#include "aria/abi/export.hpp"
#include "aria/abi/version.hpp"
#include "aria/binding/binding_engine.hpp"
#include "aria/binding/view_adapter.hpp"
#include "aria/binding/view_model.hpp"

extern "C" ARIA_BINDING_API const char* aria_binding_version() noexcept {
    return ARIA_VERSION_STRING;
}

// ── BindingEngine out-of-line definitions ───────────────────────────────────
// These are moved out of the header so that the DLL properly exports them
// (the class is marked ARIA_BINDING_API → dllimport/dllexport). Template
// methods (bind_command, bind_text_converted*, dispatch_to_view_,
// bind_scalar_oneway_/twoway_) stay in the header.

namespace aria::binding {

// ── Constructors ──────────────────────────────────────────────────────────
BindingEngine::BindingEngine(std::shared_ptr<IViewAdapter> adapter)
    : adapter_(std::move(adapter)) {}

BindingEngine::BindingEngine(std::shared_ptr<IViewAdapter> adapter,
                              std::shared_ptr<runtime::IDispatcher> ui_dispatcher,
                              DispatchPolicy policy)
    : adapter_(std::move(adapter)),
      dispatcher_(std::move(ui_dispatcher)),
      policy_(policy) {}

// ── Text ──────────────────────────────────────────────────────────────────
void BindingEngine::bind_text_oneway(Property<std::string>& prop, IView& view) {
    bind_scalar_oneway_<std::string>(prop, view, &IViewAdapter::set_text);
}

void BindingEngine::bind_text(Property<std::string>& prop, IView& view) {
    bind_scalar_two_way_<std::string>(prop, view,
        &IViewAdapter::set_text,
        &IViewAdapter::on_text_changed,
        [](std::string_view sv) { return std::string(sv); });
}

// ── Bool ──────────────────────────────────────────────────────────────────
void BindingEngine::bind_bool_oneway(Property<bool>& prop, IView& view) {
    bind_scalar_oneway_<bool>(prop, view, &IViewAdapter::set_bool);
}

void BindingEngine::bind_bool(Property<bool>& prop, IView& view) {
    bind_scalar_two_way_<bool>(prop, view,
        &IViewAdapter::set_bool,
        &IViewAdapter::on_bool_changed,
        [](bool v) { return v; });
}

// ── Int ───────────────────────────────────────────────────────────────────
void BindingEngine::bind_int_oneway(Property<int>& prop, IView& view) {
    bind_scalar_oneway_<int>(prop, view, &IViewAdapter::set_int);
}

void BindingEngine::bind_int(Property<int>& prop, IView& view) {
    bind_scalar_two_way_<int>(prop, view,
        &IViewAdapter::set_int,
        &IViewAdapter::on_int_changed,
        [](int v) { return v; });
}

// ── Int64 ─────────────────────────────────────────────────────────────────
void BindingEngine::bind_int64_oneway(Property<std::int64_t>& prop, IView& view) {
    bind_scalar_oneway_<std::int64_t>(prop, view, &IViewAdapter::set_int64);
}

void BindingEngine::bind_int64(Property<std::int64_t>& prop, IView& view) {
    bind_scalar_two_way_<std::int64_t>(prop, view,
        &IViewAdapter::set_int64,
        &IViewAdapter::on_int64_changed,
        [](std::int64_t v) { return v; });
}

// ── UInt64 ────────────────────────────────────────────────────────────────
void BindingEngine::bind_uint64_oneway(Property<std::uint64_t>& prop, IView& view) {
    bind_scalar_oneway_<std::uint64_t>(prop, view, &IViewAdapter::set_uint64);
}

void BindingEngine::bind_uint64(Property<std::uint64_t>& prop, IView& view) {
    bind_scalar_two_way_<std::uint64_t>(prop, view,
        &IViewAdapter::set_uint64,
        &IViewAdapter::on_uint64_changed,
        [](std::uint64_t v) { return v; });
}

// ── Float ─────────────────────────────────────────────────────────────────
void BindingEngine::bind_float_oneway(Property<float>& prop, IView& view) {
    bind_scalar_oneway_<float>(prop, view, &IViewAdapter::set_float);
}

void BindingEngine::bind_float(Property<float>& prop, IView& view) {
    bind_scalar_two_way_<float>(prop, view,
        &IViewAdapter::set_float,
        &IViewAdapter::on_float_changed,
        [](float v) { return v; });
}

// ── Double ────────────────────────────────────────────────────────────────
void BindingEngine::bind_double_oneway(Property<double>& prop, IView& view) {
    bind_scalar_oneway_<double>(prop, view, &IViewAdapter::set_double);
}

void BindingEngine::bind_double(Property<double>& prop, IView& view) {
    bind_scalar_two_way_<double>(prop, view,
        &IViewAdapter::set_double,
        &IViewAdapter::on_double_changed,
        [](double v) { return v; });
}

// ── Visible / Enabled ─────────────────────────────────────────────────────
void BindingEngine::bind_visible(Property<bool>& prop, IView& view) {
    bind_scalar_oneway_<bool>(prop, view, &IViewAdapter::set_visible);
}

void BindingEngine::bind_enabled(Property<bool>& prop, IView& view) {
    bind_scalar_oneway_<bool>(prop, view, &IViewAdapter::set_enabled);
}

// ── Lifecycle ─────────────────────────────────────────────────────────────
//
// `clear()` must release every active binding *and* sever every
// in-flight VM→View callable that has already been handed to the
// dispatcher. The latter is guaranteed by also releasing the
// per-view alive sentinels: posted lambdas hold a `weak_ptr<int>`
// to that sentinel and weak-lock it before touching the view, so
// once we drop our last strong ref here, every queued lambda
// becomes a no-op the moment the dispatcher pumps it.
//
// Order matters: drop the per-view buckets first (those hold one
// strong ref to each sentinel), then drop the alive-sentinel map
// (which holds the only other strong ref), then drop
// `engine_holders_`. This order keeps any concurrent observer from
// briefly seeing a "bucket cleared but sentinel alive" state.
void BindingEngine::clear() noexcept {
    per_view_.clear();
    view_alive_.clear();
    engine_holders_.clear();
}

// ── Private helpers ───────────────────────────────────────────────────────
BindingEngine::AliveToken BindingEngine::ensure_alive_token_(IView& view) {
    ViewBucket& bucket = bucket_for_(view);
    auto it = view_alive_.find(&view);
    if (it != view_alive_.end()) {
        return AliveToken{it->second};
    }
    auto sentinel = std::make_shared<int>(0);
    view_alive_.emplace(&view, sentinel);
    bucket->push_back(Subscription{sentinel});
    return AliveToken{sentinel};
}

void BindingEngine::add_view_sub_(IView& view, Subscription sub) {
    ViewBucket& bucket = bucket_for_(view);
    bucket->push_back(std::move(sub));
}

BindingEngine::ViewBucket& BindingEngine::bucket_for_(IView& view) {
    if (auto it = per_view_.find(&view); it != per_view_.end()) {
        return it->second;
    }

    // Build everything in local variables FIRST so a partially-failing
    // sequence (allocation, on_destroy connect, sub bag push_back ...)
    // can never leave the engine in a state where `per_view_` already
    // has an entry but its bucket is null or its destroy listener is
    // missing. We only mutate engine state once every fallible step
    // has succeeded.
    auto bucket_local = std::make_shared<std::vector<Subscription>>();

    Subscription bucket_holder{bucket_local};
    Subscription destroy_holder;
    {
        std::weak_ptr<std::vector<Subscription>> weak_bucket = bucket_local;
        IView* view_ptr = &view;
        destroy_holder = view.on_destroy(
            [this, view_ptr, weak_bucket]() noexcept {
                if (::aria::has_trace_sink()) {
                    try {
                        ::aria::publish_trace_unchecked(::aria::TraceCategory::Binding,
                            ::aria::trace::Binding{
                                std::string{adapter_->platform_name()},
                                std::string{},
                                "view_destroyed",
                            });
                    } catch (...) { /* never propagate from noexcept callback */ }
                }
                if (auto bucket = weak_bucket.lock()) {
                    bucket->clear();
                }
                view_alive_.erase(view_ptr);
                per_view_.erase(view_ptr);
            });
    }

    // From here on we mutate the engine. `try_emplace` won't insert a
    // null bucket because `bucket_local` is already a real shared_ptr,
    // and we only commit `engine_holders_` after the map insertion
    // succeeds (so a `bad_alloc` during emplace can't leave a dangling
    // SubscriptionBag entry pointing at a never-mapped bucket).
    auto [it, inserted] = per_view_.try_emplace(&view, bucket_local);
    if (!inserted) {
        // A concurrent (or recursive) caller already created the bucket.
        // Drop our locals — they auto-disconnect the still-uncommitted
        // on_destroy listener, no leftover state.
        return it->second;
    }
    try {
        engine_holders_ += std::move(bucket_holder);
        engine_holders_ += std::move(destroy_holder);
    } catch (...) {
        // SubscriptionBag::add can throw on push_back. Roll back the
        // map insertion so subsequent calls retry from a clean slate.
        per_view_.erase(it);
        throw;
    }
    return it->second;
}

}  // namespace aria::binding

// These dtors are defined out-of-line so the vtables are emitted inside
// the binding library exactly once (matters on Windows DLL builds and
// reduces object size on all platforms).
//
// Guard rationale (do NOT add `|| ARIA_BINDING_STATIC`):
//   * cmake SHARED/STATIC path: CMakeLists.txt always defines
//     `ARIA_BINDING_BUILD` PRIVATE on aria_binding → dtors compile into
//     the library exactly once.
//   * Windows SHARED consumer: external exe defines ARIA_BINDING_STATIC
//     (or nothing). It must NOT re-compile these dtors, otherwise
//     linker sees duplicate symbols vs aria_binding.dll.
//   * Apple .xcodeproj that inlines binding.cpp as source: should
//     define `ARIA_BINDING_BUILD=1` in GCC_PREPROCESSOR_DEFINITIONS so
//     the dtors end up in the app binary.
#if defined(ARIA_BINDING_BUILD)
namespace aria::binding {

struct IView::Impl {
    mutable ::aria::detail::TypedSignal<> destroy_signal;
    mutable bool fired = false;
};

IView::IView() : impl_(std::make_unique<Impl>()) {}
IView::~IView() { fire_destroy_(); }

Subscription IView::on_destroy(std::function<void()> cb) const {
    return impl_->destroy_signal.connect(
        [cb = std::move(cb)]() { cb(); });
}

void IView::fire_destroy_() noexcept {
    if (!impl_->fired) {
        impl_->fired = true;
        impl_->destroy_signal.emit();
    }
}

IViewAdapter::~IViewAdapter() = default;
}  // namespace aria::binding
#endif

