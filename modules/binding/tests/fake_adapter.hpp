#pragma once

#include "aria/binding/view_adapter.hpp"
#include "aria/detail/typed_signal.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace aria::binding::testing {

/// Fake widget that holds string/bool/int/int64/uint64/float/double
/// state and exposes one signal per type.
struct FakeView : public IView {
    std::string   text;
    bool          flag    = false;
    int           integer = 0;
    std::int64_t  i64     = 0;
    std::uint64_t u64     = 0;
    float         f32     = 0.0f;
    double        number  = 0.0;
    bool          visible = true;
    bool          enabled = true;

    ::aria::detail::TypedSignal<std::string>   sig_text;
    ::aria::detail::TypedSignal<bool>          sig_bool;
    ::aria::detail::TypedSignal<int>           sig_int;
    ::aria::detail::TypedSignal<std::int64_t>  sig_int64;
    ::aria::detail::TypedSignal<std::uint64_t> sig_uint64;
    ::aria::detail::TypedSignal<float>         sig_float;
    ::aria::detail::TypedSignal<double>        sig_double;
    ::aria::detail::TypedSignal<>              sig_click;

    [[nodiscard]] std::string_view kind() const noexcept override { return "fake"; }
};

/// Adapter that talks to FakeView.
class FakeAdapter : public IViewAdapter {
public:
    [[nodiscard]] std::string_view platform_name() const noexcept override { return "Fake"; }

    void set_text(IView& v, std::string_view t) override {
        auto& fv = static_cast<FakeView&>(v);
        std::string s(t);
        if (fv.text != s) {
            fv.text = std::move(s);
            fv.sig_text.emit(fv.text);
        }
    }
    std::string get_text(IView& v) override { return static_cast<FakeView&>(v).text; }
    ::aria::Subscription on_text_changed(IView& v, std::function<void(std::string_view)> cb) override {
        return static_cast<FakeView&>(v).sig_text.connect(
            [cb = std::move(cb)](const std::string& s) { cb(s); });
    }

    void set_bool(IView& v, bool b) override {
        auto& fv = static_cast<FakeView&>(v);
        if (fv.flag != b) { fv.flag = b; fv.sig_bool.emit(b); }
    }
    bool get_bool(IView& v) override { return static_cast<FakeView&>(v).flag; }
    ::aria::Subscription on_bool_changed(IView& v, std::function<void(bool)> cb) override {
        return static_cast<FakeView&>(v).sig_bool.connect(std::move(cb));
    }

    void set_int(IView& v, int n) override {
        auto& fv = static_cast<FakeView&>(v);
        if (fv.integer != n) { fv.integer = n; fv.sig_int.emit(n); }
    }
    int get_int(IView& v) override { return static_cast<FakeView&>(v).integer; }
    ::aria::Subscription on_int_changed(IView& v, std::function<void(int)> cb) override {
        return static_cast<FakeView&>(v).sig_int.connect(std::move(cb));
    }

    void set_int64(IView& v, std::int64_t n) override {
        auto& fv = static_cast<FakeView&>(v);
        if (fv.i64 != n) { fv.i64 = n; fv.sig_int64.emit(n); }
    }
    std::int64_t get_int64(IView& v) override { return static_cast<FakeView&>(v).i64; }
    ::aria::Subscription on_int64_changed(IView& v,
                                          std::function<void(std::int64_t)> cb) override {
        return static_cast<FakeView&>(v).sig_int64.connect(std::move(cb));
    }

    void set_uint64(IView& v, std::uint64_t n) override {
        auto& fv = static_cast<FakeView&>(v);
        if (fv.u64 != n) { fv.u64 = n; fv.sig_uint64.emit(n); }
    }
    std::uint64_t get_uint64(IView& v) override { return static_cast<FakeView&>(v).u64; }
    ::aria::Subscription on_uint64_changed(IView& v,
                                           std::function<void(std::uint64_t)> cb) override {
        return static_cast<FakeView&>(v).sig_uint64.connect(std::move(cb));
    }

    void set_float(IView& v, float f) override {
        auto& fv = static_cast<FakeView&>(v);
        if (fv.f32 != f) { fv.f32 = f; fv.sig_float.emit(f); }
    }
    float get_float(IView& v) override { return static_cast<FakeView&>(v).f32; }
    ::aria::Subscription on_float_changed(IView& v, std::function<void(float)> cb) override {
        return static_cast<FakeView&>(v).sig_float.connect(std::move(cb));
    }

    void set_double(IView& v, double d) override {
        auto& fv = static_cast<FakeView&>(v);
        if (fv.number != d) { fv.number = d; fv.sig_double.emit(d); }
    }
    double get_double(IView& v) override { return static_cast<FakeView&>(v).number; }
    ::aria::Subscription on_double_changed(IView& v, std::function<void(double)> cb) override {
        return static_cast<FakeView&>(v).sig_double.connect(std::move(cb));
    }

    void set_visible(IView& v, bool b) override { static_cast<FakeView&>(v).visible = b; }
    void set_enabled(IView& v, bool b) override { static_cast<FakeView&>(v).enabled = b; }

    ::aria::Subscription on_click(IView& v, std::function<void()> cb) override {
        return static_cast<FakeView&>(v).sig_click.connect(
            [cb = std::move(cb)]() { cb(); });
    }

    /// Test helper: simulate user typing.
    static void user_type(FakeView& v, std::string text) {
        v.text = std::move(text);
        v.sig_text.emit(v.text);
    }
    /// Test helper: simulate user clicking.
    static void user_click(FakeView& v) { v.sig_click.emit(); }
    /// Test helper: simulate user toggling.
    static void user_toggle(FakeView& v, bool b) {
        v.flag = b;
        v.sig_bool.emit(b);
    }
};

}  // namespace aria::binding::testing
