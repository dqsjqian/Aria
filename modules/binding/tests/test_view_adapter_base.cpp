// ============================================================================
//  test_view_adapter_base.cpp
// ----------------------------------------------------------------------------
//  `ViewAdapterBase` — the opt-in base that defaults every IViewAdapter
//  operation to the compliant "unsupported" path, so a new adapter overrides
//  only what its platform actually supports instead of all 25 pure virtuals.
//
//  Two things must hold:
//    1. a minimal adapter (text + click only) is a complete IViewAdapter and
//       real bindings work through it;
//    2. a binding that reaches an *unimplemented* channel degrades per
//       contract L-39 — reported through the diagnostics boundary, safe
//       default returned, no crash.
// ============================================================================

#include <doctest/doctest.h>

#include "aria/binding/binding_engine.hpp"
#include "aria/binding/view_adapter_base.hpp"
#include "aria/detail/typed_signal.hpp"
#include "aria/runtime/logger.hpp"
#include "aria/property.hpp"

#include <memory>
#include <string>
#include <vector>

using namespace aria;
using namespace aria::binding;

namespace {

struct MiniView : IView {
    std::string text;
    ::aria::detail::TypedSignal<std::string> sig_text;
    ::aria::detail::TypedSignal<>            sig_click;

    [[nodiscard]] std::string_view kind() const noexcept override { return "mini"; }
};

/// The whole point of the base: a real adapter in ~4 methods instead of 25.
class MiniAdapter final : public ViewAdapterBase {
public:
    [[nodiscard]] std::string_view platform_name() const noexcept override {
        return "mini";
    }

    void set_text(IView& v, std::string_view t) override {
        auto& mv = static_cast<MiniView&>(v);
        std::string s(t);
        if (mv.text != s) {
            mv.text = std::move(s);
            mv.sig_text.emit(mv.text);
        }
    }

    [[nodiscard]] std::string get_text(IView& v) override {
        return static_cast<MiniView&>(v).text;
    }

    Subscription on_text_changed(IView& v,
                                 std::function<void(std::string_view)> cb) override {
        auto& mv = static_cast<MiniView&>(v);
        return mv.sig_text.connect([cb = std::move(cb)](const std::string& s) { cb(s); });
    }

    Subscription on_click(IView& v, std::function<void()> cb) override {
        auto& mv = static_cast<MiniView&>(v);
        return mv.sig_click.connect(std::move(cb));
    }

    // Everything else — bool / int / int64 / uint64 / float / double /
    // visible / enabled — is inherited from ViewAdapterBase.
};

class LogCapture {
public:
    LogCapture() {
        messages().clear();
        auto& logger = aria::runtime::Logger::instance();
        previous_level_ = logger.level();
        logger.set_level(aria::runtime::LogLevel::Trace);
        logger.set_sink([](aria::runtime::LogLevel,
                           std::string_view category,
                           std::string_view message) {
            messages().emplace_back(std::string(category) + "|"
                                    + std::string(message));
        });
    }
    ~LogCapture() {
        auto& logger = aria::runtime::Logger::instance();
        logger.set_sink({});
        logger.set_level(previous_level_);
    }

    static std::vector<std::string>& messages() {
        static std::vector<std::string> m;
        return m;
    }

    [[nodiscard]] static bool saw(std::string_view needle) {
        for (const auto& m : messages()) {
            if (m.find(needle) != std::string::npos) return true;
        }
        return false;
    }

private:
    aria::runtime::LogLevel previous_level_{};
};

}  // namespace

TEST_CASE("ViewAdapterBase: a 4-method adapter is a complete IViewAdapter") {
    auto adapter = std::make_shared<MiniAdapter>();
    BindingEngine engine(adapter);

    Property<std::string> name("Alice");
    MiniView view;

    engine.bind_text(name, view);
    CHECK(view.text == "Alice");

    // VM → View
    name = "Bob";
    CHECK(view.text == "Bob");

    // View → VM
    adapter->set_text(view, "Charlie");
    CHECK(name.get() == "Charlie");
}

TEST_CASE("ViewAdapterBase: click works through the inherited surface") {
    auto adapter = std::make_shared<MiniAdapter>();
    BindingEngine engine(adapter);

    int hits = 0;
    Command<> cmd([&hits] { ++hits; });
    MiniView button;

    engine.bind_command(cmd, button);
    // bind_command also calls set_enabled, which MiniAdapter does not
    // implement — that must not prevent the click wiring from working.
    button.sig_click.emit();
    CHECK(hits == 1);
}

TEST_CASE("ViewAdapterBase: unimplemented setter reports and no-ops") {
    LogCapture capture;

    auto adapter = std::make_shared<MiniAdapter>();
    BindingEngine engine(adapter);

    Property<int> n(41);
    MiniView view;

    // MiniAdapter has no int channel: binding must not crash, and the
    // failure must be visible in diagnostics rather than silent.
    engine.bind_int_oneway(n, view);
    n = 42;

    CHECK(capture.saw("mini_adapter"));
    CHECK(capture.saw("set_int"));
    CHECK(capture.saw("view kind 'mini'"));
}

TEST_CASE("ViewAdapterBase: unimplemented getters return safe defaults") {
    LogCapture capture;
    MiniAdapter adapter;
    MiniView view;

    CHECK(adapter.get_bool(view) == false);
    CHECK(adapter.get_int(view) == 0);
    CHECK(adapter.get_int64(view) == 0);
    CHECK(adapter.get_uint64(view) == 0u);
    CHECK(adapter.get_float(view) == 0.0f);
    CHECK(adapter.get_double(view) == 0.0);

    CHECK(capture.saw("get_bool"));
    CHECK(capture.saw("get_double"));
}

TEST_CASE("ViewAdapterBase: unimplemented observers return an empty Subscription") {
    LogCapture capture;
    MiniAdapter adapter;
    MiniView view;

    auto s_bool   = adapter.on_bool_changed(view, [](bool) {});
    auto s_double = adapter.on_double_changed(view, [](double) {});

    // An empty Subscription is the documented "nothing was wired" result;
    // destroying it must be harmless.
    CHECK_FALSE(static_cast<bool>(s_bool));
    CHECK_FALSE(static_cast<bool>(s_double));
    CHECK(capture.saw("on_bool_changed"));
}

TEST_CASE("ViewAdapterBase: report_unsupported is overridable") {
    struct LoudAdapter final : ViewAdapterBase {
        mutable int calls = 0;
        [[nodiscard]] std::string_view platform_name() const noexcept override {
            return "loud";
        }
        void report_unsupported(std::string_view, const IView&) const override {
            ++calls;   // swallow the framework report, count instead
        }
    };

    LoudAdapter adapter;
    MiniView view;
    adapter.set_visible(view, true);
    adapter.set_enabled(view, false);
    CHECK(adapter.calls == 2);
}
