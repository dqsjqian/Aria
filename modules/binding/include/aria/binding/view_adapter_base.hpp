#pragma once

// ============================================================================
//  view_adapter_base.hpp
// ----------------------------------------------------------------------------
//  `ViewAdapterBase` — an opt-in base class for writing an `IViewAdapter`
//  without implementing all 25 pure virtuals.
//
//  Why
//  ---
//  `IViewAdapter` is a wide interface by design: text, bool, int, int64,
//  uint64, float, double each with `set_` / `get_` / `on_*_changed`, plus
//  `set_visible`, `set_enabled`, `on_click`, `platform_name`. That is the
//  right shape for the four shipped first-party adapters, which support most
//  of it — but it makes the floor for a *new* adapter 25 methods regardless
//  of ambition. An adapter that only drives labels and buttons still has to
//  write out every numeric channel by hand, and hand-roll its own
//  "unsupported" path for each one to satisfy contract L-39.
//
//  This base implements every operation as the compliant unsupported path
//  (warn once through the framework's diagnostics boundary, then return a
//  safe default), so an author overrides only what the platform genuinely
//  supports:
//
//      class MyAdapter final : public ViewAdapterBase {
//      public:
//          std::string_view platform_name() const noexcept override {
//              return "mytoolkit";      // still required — no sane default
//          }
//
//          void set_text(IView& v, std::string_view t) override { ... }
//          std::string get_text(IView& v) override { ... }
//          Subscription on_click(IView& v, std::function<void()> cb) override { ... }
//          // everything else: inherited, warns if a binding reaches it
//      };
//
//  `IViewAdapter` itself is unchanged — it is shipped and ABI-stable, and
//  the four first-party adapters continue to derive from it directly. This
//  base is purely additive.
//
//  Verifying an adapter
//  --------------------
//  The shared conformance battery is installed with the public headers, so a
//  third-party adapter can check itself against the same suite the built-in
//  ones run:
//
//      #include "aria/binding/testing/adapter_conformance.hpp"
//
//  See docs/cookbook/08-writing-a-view-adapter.md.
// ============================================================================

#include "aria/abi/export.hpp"
#include "aria/binding/view_adapter.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace aria::binding {

/// Base class defaulting every `IViewAdapter` operation to the compliant
/// "unsupported" behaviour required by contract L-39: report through the
/// diagnostics boundary, then return a safe default (no-op setter, zeroed
/// getter, empty `Subscription`).
///
/// `platform_name()` stays pure — there is no meaningful default for it, and
/// L-39 requires it to match `IView::kind()` exactly.
class ARIA_BINDING_API ViewAdapterBase : public IViewAdapter {
public:
    ~ViewAdapterBase() override;

    // ── Text ───────────────────────────────────────────────────────────
    void set_text(IView& v, std::string_view text) override;
    [[nodiscard]] std::string get_text(IView& v) override;
    Subscription on_text_changed(IView& v,
        std::function<void(std::string_view)> cb) override;

    // ── Bool ───────────────────────────────────────────────────────────
    void set_bool(IView& v, bool value) override;
    [[nodiscard]] bool get_bool(IView& v) override;
    Subscription on_bool_changed(IView& v,
        std::function<void(bool)> cb) override;

    // ── Int ────────────────────────────────────────────────────────────
    void set_int(IView& v, int value) override;
    [[nodiscard]] int get_int(IView& v) override;
    Subscription on_int_changed(IView& v,
        std::function<void(int)> cb) override;

    // ── Int64 ──────────────────────────────────────────────────────────
    void set_int64(IView& v, std::int64_t value) override;
    [[nodiscard]] std::int64_t get_int64(IView& v) override;
    Subscription on_int64_changed(IView& v,
        std::function<void(std::int64_t)> cb) override;

    // ── UInt64 ─────────────────────────────────────────────────────────
    void set_uint64(IView& v, std::uint64_t value) override;
    [[nodiscard]] std::uint64_t get_uint64(IView& v) override;
    Subscription on_uint64_changed(IView& v,
        std::function<void(std::uint64_t)> cb) override;

    // ── Float ──────────────────────────────────────────────────────────
    void set_float(IView& v, float value) override;
    [[nodiscard]] float get_float(IView& v) override;
    Subscription on_float_changed(IView& v,
        std::function<void(float)> cb) override;

    // ── Double ─────────────────────────────────────────────────────────
    void set_double(IView& v, double value) override;
    [[nodiscard]] double get_double(IView& v) override;
    Subscription on_double_changed(IView& v,
        std::function<void(double)> cb) override;

    // ── Visible / Enabled ──────────────────────────────────────────────
    void set_visible(IView& v, bool visible) override;
    void set_enabled(IView& v, bool enabled) override;

    // ── Click ──────────────────────────────────────────────────────────
    Subscription on_click(IView& v, std::function<void()> cb) override;

protected:
    /// Report that `op` has no implementation in this adapter.
    ///
    /// Routes through `runtime::Logger::warn` under the stable
    /// `<platform_name>_adapter` category and includes the view's `kind()`,
    /// matching contract L-39 rather than silently doing nothing.
    ///
    /// Override to change reporting (e.g. to throw during development, or to
    /// route into a platform log). Must not throw when called from an
    /// `on_*_changed` / `on_click` path, which is conceptually `noexcept`.
    virtual void report_unsupported(std::string_view op, const IView& v) const;
};

}  // namespace aria::binding
