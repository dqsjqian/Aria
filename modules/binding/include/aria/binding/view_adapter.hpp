#pragma once

#include "aria/abi/export.hpp"
#include "aria/detail/typed_signal.hpp"
#include "aria/subscription.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace aria::binding {

/// Abstract platform widget reference.
/// Each adapter wraps its native handle (QWidget*, NSView*, jobject, EM_VAL...)
/// in a concrete subclass of this base.
///
/// Lifetime
/// --------
/// Concrete views are typically owned by the platform's widget tree
/// (QWidget parent-owned, NSView retained by its superview, ...), not
/// by the binding layer. To let `BindingEngine` survive an early view
/// destruction, every `IView` emits a destroy signal from its base
/// destructor; the engine subscribes to this signal and automatically
/// releases any binding wired to a view that goes away before the
/// engine itself does.
class ARIA_BINDING_API IView {
public:
    IView();
    IView(const IView&) = delete;
    IView& operator=(const IView&) = delete;
    IView(IView&&) = delete;
    IView& operator=(IView&&) = delete;

    virtual ~IView();

    [[nodiscard]] virtual std::string_view kind() const noexcept = 0;

    /// Subscribe to "this view is about to be destroyed". The signal
    /// is emitted exactly once, at the start of `~IView`. Handlers must
    /// not touch the derived subclass's state — by the time they fire,
    /// the derived destructor has already run.
    [[nodiscard]] Subscription on_destroy(std::function<void()> cb) const;

protected:
    /// Invoked by `~IView()` to fan out the notification — this is the
    /// **last-resort** trigger. By the time the base destructor runs,
    /// the derived subclass's destructor and its members have already
    /// been destroyed, so handlers must treat the view as an opaque
    /// address only (exactly what `BindingEngine` needs: release the
    /// per-view subscription bucket, erase the map entry).
    ///
    /// Real adapters that wrap a native handle (QWidget, NSView, jobject,
    /// ...) are **strongly encouraged** to call `fire_destroy_()`
    /// themselves as soon as the native handle is torn down, while the
    /// subclass state is still valid. That way any handler which *does*
    /// want to touch derived state (e.g. unregister a native event
    /// callback) can do so safely. `fire_destroy_()` is idempotent — a
    /// later call from `~IView` becomes a no-op thanks to `fired_`.
    void fire_destroy_() noexcept;

private:
    struct Impl;
    // RAII pImpl (TypedSignal lives inside, view_adapter.cpp). `mutable`
    // because `on_destroy` is const but mutates the signal. C4251 on the
    // unique_ptr member is a false positive for an incomplete opaque
    // pointee consumed only via this module's API.
#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif
    mutable std::unique_ptr<Impl> impl_;
#ifdef _MSC_VER
#  pragma warning(pop)
#endif
};

/// Abstract platform adapter — knows how to read/write/observe widgets.
class ARIA_BINDING_API IViewAdapter {
public:
    virtual ~IViewAdapter();

    [[nodiscard]] virtual std::string_view platform_name() const noexcept = 0;

    // ── Text controls (label, line edit) ───────────────────────────
    virtual void set_text(IView& v, std::string_view text) = 0;
    [[nodiscard]] virtual std::string get_text(IView& v) = 0;
    virtual Subscription on_text_changed(IView& v,
                                             std::function<void(std::string_view)> cb) = 0;

    // ── Boolean controls (checkbox, switch) ────────────────────────
    virtual void set_bool(IView& v, bool value) = 0;
    [[nodiscard]] virtual bool get_bool(IView& v) = 0;
    virtual Subscription on_bool_changed(IView& v,
                                             std::function<void(bool)> cb) = 0;

    // ── Numeric controls (slider, spinbox, progress bar) ───────────
    //
    // `int` / `double` are the canonical fast paths. The wider /
    // narrower variants below exist for real workloads that hit their
    // limits:
    //   * int64_t  — timestamps (ms since epoch), IDs, byte counts
    //                that can exceed 2^31.
    //   * uint64_t — raw handles, hash digests, non-negative counters
    //                that need the full 64-bit range.
    //   * float    — platform controls that speak float natively
    //                (CALayer, UISlider, Metal uniforms) — avoids the
    //                implicit double↔float round-trip on every poll.
    //
    // Adapters whose native widget has no direct equivalent for a
    // wider/narrower type are welcome to implement the wider variant
    // by delegating to the narrower one with a range-check; see
    // `FakeAdapter` for the default pattern.

    virtual void set_int(IView& v, int value) = 0;
    [[nodiscard]] virtual int get_int(IView& v) = 0;
    virtual Subscription on_int_changed(IView& v,
                                            std::function<void(int)> cb) = 0;

    virtual void set_int64(IView& v, std::int64_t value) = 0;
    [[nodiscard]] virtual std::int64_t get_int64(IView& v) = 0;
    virtual Subscription on_int64_changed(IView& v,
                                              std::function<void(std::int64_t)> cb) = 0;

    virtual void set_uint64(IView& v, std::uint64_t value) = 0;
    [[nodiscard]] virtual std::uint64_t get_uint64(IView& v) = 0;
    virtual Subscription on_uint64_changed(IView& v,
                                               std::function<void(std::uint64_t)> cb) = 0;

    virtual void set_float(IView& v, float value) = 0;
    [[nodiscard]] virtual float get_float(IView& v) = 0;
    virtual Subscription on_float_changed(IView& v,
                                              std::function<void(float)> cb) = 0;

    virtual void set_double(IView& v, double value) = 0;
    [[nodiscard]] virtual double get_double(IView& v) = 0;
    virtual Subscription on_double_changed(IView& v,
                                               std::function<void(double)> cb) = 0;

    // ── Visibility / enabled state ─────────────────────────────────
    virtual void set_visible(IView& v, bool visible) = 0;
    virtual void set_enabled(IView& v, bool enabled) = 0;

    // ── Click events ───────────────────────────────────────────────
    virtual Subscription on_click(IView& v, std::function<void()> cb) = 0;
};

}  // namespace aria::binding
