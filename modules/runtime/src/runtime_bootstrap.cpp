// ============================================================================
//  runtime_bootstrap.cpp
// ----------------------------------------------------------------------------
//  Implementation of the public install_default_diagnostics() API. See
//  <aria/runtime/runtime_bootstrap.hpp> for the rationale.
// ============================================================================

#include "aria/runtime/runtime_bootstrap.hpp"

#include "aria/abi/slot_factory.hpp"
#include "aria/callback_boundary.hpp"
#include "aria/runtime/logger.hpp"

#include <atomic>
#include <exception>
#include <mutex>
#include <string>
#include <string_view>

namespace aria::runtime {

namespace {

// State for idempotence and reverse-rollback.
struct BootstrapState {
    std::mutex                     mu;
    bool                           installed = false;
    aria::CallbackFailureSink      previous_failure_sink = nullptr;
    aria::abi::SlotInvokeFailureHook previous_slot_hook   = nullptr;
};

inline BootstrapState& state_() noexcept {
    static BootstrapState s;
    return s;
}

// Render a CallbackFailure into a plain string for the logger.
std::string render_(const aria::CallbackFailure& f) {
    if (!f.message.empty()) return std::string{f.message};
    if (f.exception) {
        try {
            std::rethrow_exception(f.exception);
        } catch (const std::exception& e) {
            return std::string{e.what()};
        } catch (...) {
            return "non-std::exception";
        }
    }
    return "(no payload)";
}

// Logger-bridging sink. The category from the CallbackFailure becomes the
// logger's category tag, prefixed with "aria." so host log filters can
// route framework-internal failures cleanly.
void logger_bridge_sink_(const aria::CallbackFailure& f) {
    try {
        const std::string msg      = render_(f);
        std::string       category = "aria.";
        category.append(f.category.data(), f.category.size());
        aria::runtime::Logger::instance().error(category, msg);
    } catch (...) {
        // The bridge itself must never propagate; the framework's own
        // try/catch around the sink call will fall back to stderr if we
        // do somehow throw.
    }
}

// Slot-invoke hook: capture the in-flight exception and re-route it
// through the unified callback-failure channel. Carries category
// "abi.slot.invoke".
void slot_invoke_hook_(std::exception_ptr eptr) noexcept {
    aria::report_callback_failure(
        std::string_view{"abi.slot.invoke"}, std::move(eptr));
}

}  // namespace

bool install_default_diagnostics() noexcept {
    auto& s = state_();
    std::lock_guard lk(s.mu);
    if (s.installed) return false;

    s.previous_failure_sink =
        aria::set_callback_failure_sink(&logger_bridge_sink_);
    s.previous_slot_hook =
        aria::abi::set_slot_invoke_failure_hook(&slot_invoke_hook_);
    s.installed = true;
    return true;
}

void uninstall_default_diagnostics() noexcept {
    auto& s = state_();
    std::lock_guard lk(s.mu);
    if (!s.installed) return;

    aria::set_callback_failure_sink(s.previous_failure_sink);
    aria::abi::set_slot_invoke_failure_hook(s.previous_slot_hook);
    s.previous_failure_sink = nullptr;
    s.previous_slot_hook    = nullptr;
    s.installed             = false;
}

}  // namespace aria::runtime
