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
    : adapter_(std::move(adapter)) {
    if (!adapter_) throw std::invalid_argument("BindingEngine: adapter is null");
}

BindingEngine::BindingEngine(std::shared_ptr<IViewAdapter> adapter,
                              std::shared_ptr<runtime::IDispatcher> ui_dispatcher,
                              DispatchPolicy policy)
    : adapter_(std::move(adapter)),
      dispatcher_(std::move(ui_dispatcher)),
      policy_(policy) {
    if (!adapter_) throw std::invalid_argument("BindingEngine: adapter is null");
    if (!dispatcher_ && policy_ != DispatchPolicy::Direct) {
        throw std::invalid_argument("BindingEngine: marshalling requires a dispatcher");
    }
}

// Out-of-line on purpose: keeps the destructors of the private container
// members from being expanded inline in every consumer TU. Clear explicitly
// retires lifetime gates before subscription destructors invoke user code.
BindingEngine::~BindingEngine() {
    closing_ = true;
    clear();
}

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
void BindingEngine::clear() noexcept {
    // Detach the entire registry and retire all gates before a Subscription
    // destructor invokes user code. Reentrant bindings go into a fresh map.
    decltype(per_view_) retired;
    retired.swap(per_view_);
    for (auto& [view, bucket] : retired) bucket->active = false;
}

BindingEngine::AliveToken BindingEngine::ensure_alive_token_(IView& view) {
    return bucket_for_(view);
}

void BindingEngine::add_view_sub_(IView& view, Subscription sub) {
    bucket_for_(view)->subscriptions.push_back(std::move(sub));
}

std::shared_ptr<BindingEngine::ViewBucket> BindingEngine::bucket_for_(IView& view) {
    if (closing_) throw std::logic_error("BindingEngine: binding during destruction");
    if (auto it = per_view_.find(&view); it != per_view_.end()) return it->second;

    auto bucket = std::make_shared<ViewBucket>();
    std::weak_ptr<ViewBucket> weak = bucket;
    bucket->destroy_listener = view.on_destroy([this, view_ptr = &view, weak] {
        auto retired = weak.lock();
        if (!retired || !retired->active) return;
        retired->active = false;
        auto adapter = adapter_;
        per_view_.erase(view_ptr);
        // No engine access follows: the trace/disconnectors may destroy it.
        if (::aria::has_trace_sink()) {
            trace_binding_(adapter->platform_name(), "view_destroyed");
        }
        // Detach the listener before releasing arbitrary subscriptions; the
        // emitter retains the currently executing callback until it returns.
        retired->destroy_listener.release();
        auto subscriptions = std::move(retired->subscriptions);
    });
    auto [it, inserted] = per_view_.try_emplace(&view, bucket);
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
