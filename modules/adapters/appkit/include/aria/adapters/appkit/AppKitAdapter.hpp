#pragma once

/// AppKitAdapter — AppKit implementation of aria::binding::IViewAdapter.
///
/// Production-grade implementation that passes the full
/// `adapter_conformance` test battery. The adapter wraps:
///
///   * `NSTextField` (editable + uneditable) — text two-way binding
///   * `NSButton` with `NSSwitchButton` style — bool two-way binding
///   * `NSButton` (push button) — click events
///   * `NSStepper` — int two-way binding
///   * `NSSlider` — double two-way binding
///
/// Lifetime / ABI
/// --------------
///   * `AppKitView` retains its `NSView*` via ARC (`__strong`), so the
///     view stays alive as long as the C++ wrapper does.
///   * `~AppKitView()` calls `fire_destroy_()` while the native handle
///     is still valid — this lets `BindingEngine` drop the per-view
///     subscription bucket before the storage is reclaimed.
///   * Every observer subscription returned from on_*_changed / on_click
///     properly detaches its signal slot when released, mirroring the
///     Qt6 adapter's behaviour.
///
/// Threading
/// ---------
///   AppKit widgets live on the main thread. The adapter assumes its
///   methods are called there. Cross-thread Property writes must be
///   marshalled through `MainThreadExecutor` (or an equivalent
///   Dispatcher) before reaching the adapter.
///
/// This header is `.mm`-only — it imports Cocoa.

#include "aria/binding/view_adapter.hpp"
#include "aria/subscription.hpp"

#import <Cocoa/Cocoa.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

// ─── ObjC bridging targets (defined in AppKitAdapter.mm) ────────────────

/// Click-action target for NSButton (push buttons).
@interface AriaClickTarget : NSObject
- (instancetype)initWithCallback:(std::function<void()>)cb;
- (void)fire:(id)sender;
@end

/// Toggle-action target for NSButton (NSSwitchButton/NSCheckButton).
@interface AriaToggleTarget : NSObject
- (instancetype)initWithCallback:(std::function<void(bool)>)cb;
- (void)fire:(id)sender;
@end

/// Stepper-action target for NSStepper.
@interface AriaStepperTarget : NSObject
- (instancetype)initWithCallback:(std::function<void(int)>)cb;
- (void)fire:(id)sender;
@end

/// Slider-action target for NSSlider.
@interface AriaSliderTarget : NSObject
- (instancetype)initWithCallback:(std::function<void(double)>)cb;
- (void)fire:(id)sender;
@end

/// NSTextField delegate translating textDidChange into a C++ callback.
@interface AriaTextDelegate : NSObject <NSTextFieldDelegate>
- (instancetype)initWithCallback:(std::function<void(std::string_view)>)cb;
@end

namespace aria::adapters::appkit {

// ─── View wrapper ───────────────────────────────────────────────────────

class AppKitView : public ::aria::binding::IView {
public:
    explicit AppKitView(NSView* view) : view_(view) {}
    ~AppKitView() override {
        // Fire while the NSView* is still alive; handlers (BindingEngine)
        // can erase the per-view bucket before our storage goes away.
        fire_destroy_();
    }

    [[nodiscard]] std::string_view kind() const noexcept override { return "appkit"; }
    [[nodiscard]] NSView* native() const noexcept { return view_; }

    template<class T>
    [[nodiscard]] T* as() const { return static_cast<T*>(view_); }

private:
    NSView* __strong view_;  // ARC: retain
};

// ─── Adapter ────────────────────────────────────────────────────────────

class AppKitAdapter : public ::aria::binding::IViewAdapter {
public:
    AppKitAdapter();
    ~AppKitAdapter() override;

    AppKitAdapter(const AppKitAdapter&) = delete;
    AppKitAdapter& operator=(const AppKitAdapter&) = delete;

    [[nodiscard]] std::string_view platform_name() const noexcept override {
        return "appkit";
    }

    // ── Handle → IView ──────────────────────────────────────────────────
    /// Wrap a native `NSView*` as an `IView` owned by this adapter.
    ///
    /// Every host used to hand-roll this: allocate an `AppKitView`, then
    /// park it in a `std::vector<std::unique_ptr<AppKitView>>` because
    /// `BindingEngine` takes `IView&` and does not own its views. The
    /// adapter already keeps a per-view registry for its ObjC signal
    /// bridges, so it is the natural owner.
    ///
    ///     engine.bind_text(vm.name, adapter->view_for(nameField));
    ///
    /// Calling `view_for` twice for the same `NSView*` returns the *same*
    /// `AppKitView`, so multiple bindings on one control share a single
    /// per-view subscription bucket in `BindingEngine`.
    ///
    /// Lifetime — differs from the Qt adapter, deliberately:
    /// `AppKitView` retains its `NSView*` via ARC, so a cached view keeps
    /// the control alive and there is no "native handle died first" event
    /// to evict on. Entries therefore live until the adapter is destroyed
    /// (which fires each `IView`'s destroy signal and releases the
    /// bindings), or until `release_view` is called explicitly for a
    /// control the host is discarding early.
    ///
    /// Passing `nil` is a programming error and throws
    /// `std::invalid_argument`.
    [[nodiscard]] AppKitView& view_for(NSView* view);

    /// Drop the cached `AppKitView` for `view`, if any.
    ///
    /// Fires that view's destroy signal, so `BindingEngine` releases every
    /// binding wired to it, and releases the adapter's ARC reference to the
    /// control. Use this when a host tears down part of its UI while
    /// keeping the adapter alive (a closed panel, a recycled row). Calling
    /// it for an unknown view is a harmless no-op.
    void release_view(NSView* view) noexcept;

    // ── Text ────────────────────────────────────────────────────────────
    void                 set_text(::aria::binding::IView& v, std::string_view text) override;
    [[nodiscard]] std::string get_text(::aria::binding::IView& v) override;
    ::aria::Subscription on_text_changed(::aria::binding::IView& v,
        std::function<void(std::string_view)> cb) override;

    // ── Bool ────────────────────────────────────────────────────────────
    void                 set_bool(::aria::binding::IView& v, bool value) override;
    [[nodiscard]] bool   get_bool(::aria::binding::IView& v) override;
    ::aria::Subscription on_bool_changed(::aria::binding::IView& v,
        std::function<void(bool)> cb) override;

    // ── Int ─────────────────────────────────────────────────────────────
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

    // ── Double ──────────────────────────────────────────────────────────
    void                 set_double(::aria::binding::IView& v, double value) override;
    [[nodiscard]] double get_double(::aria::binding::IView& v) override;
    ::aria::Subscription on_double_changed(::aria::binding::IView& v,
        std::function<void(double)> cb) override;

    // ── Visibility / enabled ────────────────────────────────────────────
    void                 set_visible(::aria::binding::IView& v, bool visible) override;
    void                 set_enabled(::aria::binding::IView& v, bool enabled) override;

    // ── Click ───────────────────────────────────────────────────────────
    ::aria::Subscription on_click(::aria::binding::IView& v,
        std::function<void()> cb) override;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace aria::adapters::appkit

// ─── Backwards-compatible type alias ────────────────────────────────────
//
// Backwards-compat aliases: legacy showcase code imported
// `AppKitAdapter` / `AppKitView` from the global namespace; the
// canonical home is now `aria::adapters::appkit`. The aliases
// below keep the existing showcase compile paths working unchanged.
using AppKitAdapter = ::aria::adapters::appkit::AppKitAdapter;
using AppKitView    = ::aria::adapters::appkit::AppKitView;

// Backwards-compat ObjC type alias for the showcase that used the old name.
@compatibility_alias AppKitClickWrapper AriaClickTarget;
