#pragma once

/// UIKitAdapter — UIKit implementation of aria::binding::IViewAdapter.
///
/// Production-grade implementation that conforms to the shared
/// `adapter_conformance` test battery. The adapter wraps:
///
///   * `UITextField` — text two-way binding (UIControlEventEditingChanged)
///   * `UISwitch`    — bool two-way binding (UIControlEventValueChanged)
///   * `UIStepper`   — int two-way binding (UIControlEventValueChanged)
///   * `UISlider`    — double two-way binding (UIControlEventValueChanged)
///   * `UIButton`    — click events (UIControlEventTouchUpInside)
///
/// Lifetime / ABI
/// --------------
///   * `UIKitView` retains its `UIView*` via ARC (`__strong`).
///   * `~UIKitView()` calls `fire_destroy_()` while the native handle
///     is still valid, so BindingEngine can drop the per-view
///     subscription bucket cleanly.
///   * Subscriptions returned from on_*_changed / on_click detach
///     properly via `SignalErased::disconnect_via_weak`, mirroring
///     the Qt6 / AppKit adapters.
///
/// Threading
/// ---------
///   UIKit widgets live on the main thread. The adapter assumes its
///   methods are called there; cross-thread Property writes must be
///   marshalled through a `MainThreadExecutor` first.
///
/// Build
/// -----
///   This header is .mm-only because it imports UIKit. Consumers link the
///   opt-in `aria::adapters::uikit` CMake target from an iOS toolchain.

#include "aria/binding/view_adapter.hpp"
#include "aria/subscription.hpp"

#import <UIKit/UIKit.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

// ─── ObjC bridging targets / delegates ──────────────────────────────────

@interface AriaUIClickTarget : NSObject
- (instancetype)initWithCallback:(std::function<void()>)cb;
- (void)fire:(id)sender;
@end

@interface AriaUIToggleTarget : NSObject
- (instancetype)initWithCallback:(std::function<void(bool)>)cb;
- (void)fire:(id)sender;
@end

@interface AriaUIStepperTarget : NSObject
- (instancetype)initWithCallback:(std::function<void(int)>)cb;
- (void)fire:(id)sender;
@end

@interface AriaUISliderTarget : NSObject
- (instancetype)initWithCallback:(std::function<void(double)>)cb;
- (void)fire:(id)sender;
@end

@interface AriaUITextTarget : NSObject
- (instancetype)initWithCallback:(std::function<void(std::string_view)>)cb;
- (void)fire:(id)sender;
@end

namespace aria::adapters::uikit {

// ─── View wrapper ───────────────────────────────────────────────────────

class UIKitView : public ::aria::binding::IView {
public:
    explicit UIKitView(UIView* view) : view_(view) {}
    ~UIKitView() override {
        // Fire destroy while UIView* is still alive so BindingEngine
        // can erase its bucket before our storage is reclaimed.
        fire_destroy_();
    }

    [[nodiscard]] std::string_view kind() const noexcept override { return "uikit"; }
    [[nodiscard]] UIView* native() const noexcept { return view_; }

    template<class T>
    [[nodiscard]] T* as() const { return static_cast<T*>(view_); }

private:
    UIView* __strong view_;
};

// ─── Adapter ────────────────────────────────────────────────────────────

class UIKitAdapter : public ::aria::binding::IViewAdapter {
public:
    UIKitAdapter();
    ~UIKitAdapter() override;

    UIKitAdapter(const UIKitAdapter&) = delete;
    UIKitAdapter& operator=(const UIKitAdapter&) = delete;

    [[nodiscard]] std::string_view platform_name() const noexcept override {
        return "uikit";
    }

    // ── Handle → IView ──────────────────────────────────────────────────
    /// Wrap a native `UIView*` as an `IView` owned by this adapter.
    ///
    /// Every host used to hand-roll this: allocate a `UIKitView`, then park
    /// it in a keepalive container because `BindingEngine` takes `IView&`
    /// and does not own its views. (The common workaround — a process-global
    /// `std::vector<UIKitView>` — never releases anything, so it leaks by
    /// construction.) The adapter already keeps a per-view registry for its
    /// ObjC control targets, so it is the natural owner.
    ///
    ///     engine.bind_text(vm.name, adapter->view_for(nameField));
    ///
    /// Calling `view_for` twice for the same `UIView*` returns the *same*
    /// `UIKitView`, so multiple bindings on one control share a single
    /// per-view subscription bucket in `BindingEngine`.
    ///
    /// Lifetime — matches the AppKit adapter, and differs from Qt's on
    /// purpose: `UIKitView` retains its `UIView*` via ARC, so a cached view
    /// keeps the control alive and there is no "native handle died first"
    /// event to evict on. Entries live until the adapter is destroyed (which
    /// fires each `IView`'s destroy signal and releases the bindings), or
    /// until `release_view` is called for a control the host discards early
    /// — a popped view controller, a recycled cell.
    ///
    /// Passing `nil` is a programming error and throws
    /// `std::invalid_argument`.
    [[nodiscard]] UIKitView& view_for(UIView* view);

    /// Drop the cached `UIKitView` for `view`, if any.
    ///
    /// Fires that view's destroy signal, so `BindingEngine` releases every
    /// binding wired to it, and releases the adapter's ARC reference to the
    /// control. Calling it for an unknown view is a harmless no-op.
    void release_view(UIView* view) noexcept;

    void                 set_text(::aria::binding::IView& v, std::string_view text) override;
    [[nodiscard]] std::string get_text(::aria::binding::IView& v) override;
    ::aria::Subscription on_text_changed(::aria::binding::IView& v,
        std::function<void(std::string_view)> cb) override;

    void                 set_bool(::aria::binding::IView& v, bool value) override;
    [[nodiscard]] bool   get_bool(::aria::binding::IView& v) override;
    ::aria::Subscription on_bool_changed(::aria::binding::IView& v,
        std::function<void(bool)> cb) override;

    void                 set_int(::aria::binding::IView& v, int value) override;
    [[nodiscard]] int    get_int(::aria::binding::IView& v) override;
    ::aria::Subscription on_int_changed(::aria::binding::IView& v,
        std::function<void(int)> cb) override;

    void                 set_int64(::aria::binding::IView& v, std::int64_t value) override;
    [[nodiscard]] std::int64_t get_int64(::aria::binding::IView& v) override;
    ::aria::Subscription on_int64_changed(::aria::binding::IView& v,
        std::function<void(std::int64_t)> cb) override;

    void                 set_uint64(::aria::binding::IView& v, std::uint64_t value) override;
    [[nodiscard]] std::uint64_t get_uint64(::aria::binding::IView& v) override;
    ::aria::Subscription on_uint64_changed(::aria::binding::IView& v,
        std::function<void(std::uint64_t)> cb) override;

    void                 set_float(::aria::binding::IView& v, float value) override;
    [[nodiscard]] float  get_float(::aria::binding::IView& v) override;
    ::aria::Subscription on_float_changed(::aria::binding::IView& v,
        std::function<void(float)> cb) override;

    void                 set_double(::aria::binding::IView& v, double value) override;
    [[nodiscard]] double get_double(::aria::binding::IView& v) override;
    ::aria::Subscription on_double_changed(::aria::binding::IView& v,
        std::function<void(double)> cb) override;

    void                 set_visible(::aria::binding::IView& v, bool visible) override;
    void                 set_enabled(::aria::binding::IView& v, bool enabled) override;

    ::aria::Subscription on_click(::aria::binding::IView& v,
        std::function<void()> cb) override;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace aria::adapters::uikit

// ─── Backwards-compat aliases ───────────────────────────────────────────

using UIKitAdapter = ::aria::adapters::uikit::UIKitAdapter;
using UIKitView    = ::aria::adapters::uikit::UIKitView;

@compatibility_alias UIKitClickWrapper AriaUIClickTarget;
