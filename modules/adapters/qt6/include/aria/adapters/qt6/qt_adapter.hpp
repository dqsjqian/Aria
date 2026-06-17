#pragma once

#include "aria/abi/export.hpp"
#include "aria/binding/view_adapter.hpp"
#include "qt_view.hpp"

#include <cstdint>
#include <memory>

namespace aria::adapters::qt6 {

/// Qt6 implementation of IViewAdapter.
///
/// Supports out of the box:
///   - QLabel / QLineEdit / QPlainTextEdit / QTextEdit  (text)
///   - QCheckBox / QRadioButton / QAbstractButton (bool)
///   - QSpinBox / QSlider / QDial / QProgressBar (int)
///   - QDoubleSpinBox (double)
///   - QPushButton / QToolButton (click)
///   - QWidget   (visible / enabled)
///
/// Internally we own a small per-adapter signal registry so the Subscription
/// objects returned to user code are the unified ::aria::Subscription
/// used everywhere else — RAII-safe even if the QWidget gets destroyed first.
class ARIA_QT6_API QtAdapter final : public binding::IViewAdapter {
public:
    QtAdapter();
    ~QtAdapter() override;

    [[nodiscard]] std::string_view platform_name() const noexcept override {
        return "qt6";
    }

    // ── Text ───────────────────────────────────────────────────────────────
    void set_text(binding::IView& v, std::string_view text) override;
    [[nodiscard]] std::string get_text(binding::IView& v) override;
    ::aria::Subscription on_text_changed(binding::IView& v,
                                     std::function<void(std::string_view)> cb) override;

    // ── Bool ───────────────────────────────────────────────────────────────
    void set_bool(binding::IView& v, bool value) override;
    [[nodiscard]] bool get_bool(binding::IView& v) override;
    ::aria::Subscription on_bool_changed(binding::IView& v,
                                     std::function<void(bool)> cb) override;

    // ── Int ────────────────────────────────────────────────────────────────
    void set_int(binding::IView& v, int value) override;
    [[nodiscard]] int get_int(binding::IView& v) override;
    ::aria::Subscription on_int_changed(binding::IView& v,
                                    std::function<void(int)> cb) override;

    // ── Int64 ──────────────────────────────────────────────────────────────
    // Qt widgets don't speak int64 natively; these forward to set_int/get_int
    // with a narrowing cast guarded by assert-in-debug. Hosts that need the
    // full 64-bit range (timestamps, big IDs) should use a QLineEdit bound
    // via bind_text_converted with a user-supplied string<->int64 converter.
    void set_int64(binding::IView& v, std::int64_t value) override;
    [[nodiscard]] std::int64_t get_int64(binding::IView& v) override;
    ::aria::Subscription on_int64_changed(binding::IView& v,
                                      std::function<void(std::int64_t)> cb) override;

    // ── UInt64 ─────────────────────────────────────────────────────────────
    // Same forwarding strategy as int64 — Qt numeric widgets top out at int.
    void set_uint64(binding::IView& v, std::uint64_t value) override;
    [[nodiscard]] std::uint64_t get_uint64(binding::IView& v) override;
    ::aria::Subscription on_uint64_changed(binding::IView& v,
                                       std::function<void(std::uint64_t)> cb) override;

    // ── Float ──────────────────────────────────────────────────────────────
    // Forwards to set_double/get_double; the narrowing is harmless for the
    // QDoubleSpinBox range.
    void set_float(binding::IView& v, float value) override;
    [[nodiscard]] float get_float(binding::IView& v) override;
    ::aria::Subscription on_float_changed(binding::IView& v,
                                      std::function<void(float)> cb) override;

    // ── Double ─────────────────────────────────────────────────────────────
    void set_double(binding::IView& v, double value) override;
    [[nodiscard]] double get_double(binding::IView& v) override;
    ::aria::Subscription on_double_changed(binding::IView& v,
                                       std::function<void(double)> cb) override;

    // ── Visibility / enabled ───────────────────────────────────────────────
    void set_visible(binding::IView& v, bool visible) override;
    void set_enabled(binding::IView& v, bool enabled) override;

    // ── Click ──────────────────────────────────────────────────────────────
    ::aria::Subscription on_click(binding::IView& v, std::function<void()> cb) override;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace aria::adapters::qt6
