// ============================================================================
//  test_callback_boundary.cpp
// ----------------------------------------------------------------------------
//  Pin down the contract of aria::report_callback_failure: a single, never-
//  throwing reporting channel that funnels exceptions escaping framework-
//  internal user-callback boundaries into a process-wide sink.
// ============================================================================

#include <doctest/doctest.h>

#include "aria/callback_boundary.hpp"

#include <atomic>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

// One-shot capture used by every test below. We keep it process-global so
// the function-pointer sink can refer to it without capturing state.
struct Capture {
    std::vector<std::string> categories;
    std::vector<std::string> messages;
    std::atomic<int>         calls{0};
    bool                     should_throw{false};

    void reset() noexcept {
        categories.clear();
        messages.clear();
        calls.store(0, std::memory_order_release);
        should_throw = false;
    }
};

inline Capture& capture() {
    static Capture c;
    return c;
}

void capture_sink(const aria::CallbackFailure& f) noexcept {
    auto& c = capture();
    c.categories.emplace_back(f.category);
    c.messages.emplace_back(
        f.message.empty()
            ? std::string{}
            : std::string{f.message});
    c.calls.fetch_add(1, std::memory_order_acq_rel);
    if (c.should_throw) {
        // Simulate a misbehaving sink. We cannot actually throw out of a
        // noexcept function — that would call std::terminate — so we
        // emulate the failure by toggling the flag and trusting the
        // framework's secondary catch path. CB-5 verifies the path by
        // making the sink throw via a NON-noexcept indirection; see the
        // dedicated test.
    }
}

// Throwing variant used by CB-5 only. Declared NOT noexcept so its raise
// reaches `report_callback_failure`'s internal `catch (...)`. We avoid
// `[[gnu::noinline]]` here so MSVC does not warn about the unknown
// attribute (C5030); whether the function is inlined or not has no
// bearing on what CB-5 verifies.
void throwing_sink(const aria::CallbackFailure& f) {
    auto& c = capture();
    c.categories.emplace_back(f.category);
    c.messages.emplace_back(
        f.message.empty()
            ? std::string{}
            : std::string{f.message});
    c.calls.fetch_add(1, std::memory_order_acq_rel);
    throw std::runtime_error{"sink misbehaved"};
}

class SinkScope {
public:
    SinkScope() noexcept
        : previous_(aria::set_callback_failure_sink(&capture_sink)) {
        capture().reset();
    }
    ~SinkScope() noexcept {
        aria::set_callback_failure_sink(previous_);
    }

    SinkScope(const SinkScope&)            = delete;
    SinkScope& operator=(const SinkScope&) = delete;

private:
    aria::CallbackFailureSink previous_;
};

}  // namespace

// ----------------------------------------------------------------------------
//  CB-1: default state — no sink installed, fallback path taken.
// ----------------------------------------------------------------------------
TEST_CASE("callback_boundary: default state has no sink installed") {
    // Don't install one; just check the read.
    CHECK(aria::current_callback_failure_sink() == nullptr);
}

// ----------------------------------------------------------------------------
//  CB-2: install and exchange returns previous.
// ----------------------------------------------------------------------------
TEST_CASE("callback_boundary: set_callback_failure_sink returns previous") {
    auto* prev = aria::set_callback_failure_sink(&capture_sink);
    CHECK(aria::current_callback_failure_sink() == &capture_sink);
    auto* prev2 = aria::set_callback_failure_sink(prev);
    CHECK(prev2 == &capture_sink);
    CHECK(aria::current_callback_failure_sink() == prev);
}

// ----------------------------------------------------------------------------
//  CB-3: installed sink receives category + message.
// ----------------------------------------------------------------------------
TEST_CASE("callback_boundary: report routes to installed sink") {
    SinkScope scope;
    aria::report_callback_failure(
        std::string_view{"executor.thread_pool.worker"},
        std::string_view{"deliberate test"});
    REQUIRE(capture().calls.load() == 1);
    CHECK(capture().categories[0] == "executor.thread_pool.worker");
    CHECK(capture().messages[0]   == "deliberate test");
}

// ----------------------------------------------------------------------------
//  CB-4: report carries an exception_ptr; sink can rethrow & inspect.
// ----------------------------------------------------------------------------
TEST_CASE("callback_boundary: report carries an exception_ptr") {
    static std::string seen_what;
    static aria::CallbackFailureSink prev = nullptr;

    struct Local {
        static void inspect_sink(const aria::CallbackFailure& f) noexcept {
            try {
                if (f.exception) std::rethrow_exception(f.exception);
            } catch (const std::exception& e) {
                seen_what = e.what();
            } catch (...) {
                seen_what = "non-std";
            }
        }
    };

    seen_what.clear();
    prev = aria::set_callback_failure_sink(&Local::inspect_sink);
    try {
        try {
            throw std::runtime_error{"hello from boundary"};
        } catch (...) {
            aria::report_callback_failure(
                std::string_view{"abi.slot.invoke"},
                std::current_exception());
        }
        CHECK(seen_what == "hello from boundary");
    } catch (...) {
        aria::set_callback_failure_sink(prev);
        throw;
    }
    aria::set_callback_failure_sink(prev);
}

// ----------------------------------------------------------------------------
//  CB-5: a throwing sink does not propagate.
// ----------------------------------------------------------------------------
TEST_CASE("callback_boundary: throwing sink is contained") {
    auto* prev = aria::set_callback_failure_sink(&throwing_sink);
    capture().reset();
    // Must NOT throw out of the noexcept reporter.
    CHECK_NOTHROW(aria::report_callback_failure(
        std::string_view{"runtime.simple_dispatcher.pump"},
        std::string_view{"this sink throws"}));
    // Sink itself was hit even though it raised.
    CHECK(capture().calls.load() == 1);
    aria::set_callback_failure_sink(prev);
}

// ----------------------------------------------------------------------------
//  CB-6: a null exception_ptr with empty message yields a "(no payload)"
//  rendering on the default sink. We verify by stubbing the sink and
//  letting the default fallback render through `render_message_`.
//
//  We can't directly observe stderr; instead we install a sink that asks
//  the same question — "given an empty message and a null exception, does
//  the framework treat that as a well-formed report?" — and confirm the
//  inputs reach the sink unchanged.
// ----------------------------------------------------------------------------
TEST_CASE("callback_boundary: empty payload reaches the sink") {
    SinkScope scope;
    aria::report_callback_failure(
        std::string_view{"executor.main_thread.drain"},
        std::exception_ptr{},
        std::string_view{});
    REQUIRE(capture().calls.load() == 1);
    CHECK(capture().categories[0] == "executor.main_thread.drain");
    CHECK(capture().messages[0].empty());
}
