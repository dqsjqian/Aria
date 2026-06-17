#pragma once

// aria::CallbackBoundary — the single, framework-wide reporting channel for
// exceptions that escape a synchronous user callback at a boundary the
// framework MUST keep moving past.
//
// Why this exists
// ---------------
// Several locations inside the framework run user code in contexts where an
// exception cannot be propagated to a meaningful caller:
//
//   * worker threads inside a thread-pool executor;
//   * the main-thread executor / dispatcher's drain / pump loops;
//   * the ABI slot trampoline that backs `abi::SignalErased::emit`;
//   * the virtual-time executor's pump (test-side scheduler);
//   * detached coroutines (already routed through `aria::async`).
//
// Historically each site ended in a bare `catch (...) { /* swallow */ }`,
// which made production failures invisible. This header defines a single
// reporting primitive that those sites all funnel into:
//
//   aria::report_callback_failure("executor.thread_pool.worker",
//                                 std::current_exception());
//
// The host application installs ONE sink with `set_callback_failure_sink`
// (typically routed into the logger). Until then the default sink prints a
// single line to `stderr` so production failures are never invisible.
//
// Lifetime / threading
// --------------------
// * The sink is a function-pointer-based registration stored in an atomic
//   variable. Installation, replacement, and read are all lock-free.
// * The reporter is `noexcept`: a sink that throws is itself caught and
//   degraded to a `stderr` fallback so the framework's noexcept boundaries
//   stay honest.
// * `std::current_exception()` is captured at the boundary, so each report
//   carries a structured exception_ptr the sink can rethrow if it wants
//   typed inspection — ABI / type-erased payloads are not required.
//
// Layering
// --------
// This lives in `core` because both `async` (`async_error_sink`) and the
// concrete dispatcher / executor implementations need it. `runtime` injects
// a default sink that routes into `aria::Logger`. Adapters and binding
// layers may, but need not, install their own sinks.

#include "aria/abi/export.hpp"
#include "aria/function_ref.hpp"

#include <atomic>
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace aria {

/// Payload passed to a callback-failure sink. `category` is a short, dotted
/// identifier of the boundary (e.g. `"executor.thread_pool.worker"`) so
/// host applications can split logs / counters by source. `exception` may
/// be null if the boundary observed an error without an exception object
/// (rare; reserved for future use). `message` carries an optional precomputed
/// user-readable summary; if empty the sink should derive one by rethrowing
/// + catching the exception_ptr.
struct CallbackFailure {
    std::string_view   category;
    std::exception_ptr exception;
    std::string_view   message;
};

/// Sink type. The framework wraps every invocation in `try / catch (...)`,
/// so a sink that throws will not crash the noexcept reporter — the
/// framework swallows the secondary failure and falls through to the
/// stderr fallback. By contract the sink **should** still be noexcept;
/// non-noexcept is allowed only because we cannot make exception safety
/// observable on a function-pointer type alone.
using CallbackFailureSink = void (*)(const CallbackFailure&);

namespace detail::callback_boundary {

// Storage lives in libaria_abi and is exported through this accessor.
// In the shipped link graph `libaria_abi` is a static archive that is
// linked exclusively into `libaria_runtime`; every other shared
// consumer (binding, platform adapters, host exe) reaches this same
// physical slot by resolving against `libaria_runtime`. That is what
// gives the framework a single per-process slot — NOT "static archive
// linked into every shared consumer". See callback_boundary.cpp for
// the constraints this places on future build-graph changes.
ARIA_ABI_API std::atomic<CallbackFailureSink>& sink_storage() noexcept;

inline std::string render_message_(const CallbackFailure& f) {
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

inline void default_sink_(const CallbackFailure& f) noexcept {
    // Fallback: a single, parseable line on stderr. Host apps that want
    // structured logging install their own sink during runtime startup.
    try {
        const std::string msg = render_message_(f);
        std::fprintf(stderr, "[aria.callback_failure] %.*s: %s\n",
                     static_cast<int>(f.category.size()), f.category.data(),
                     msg.c_str());
        std::fflush(stderr);
    } catch (...) {
        // We are already a noexcept boundary of last resort. Even formatting
        // can fail under OOM; in that case we simply give up — the framework
        // contract is to never raise from a noexcept boundary.
    }
}

}  // namespace detail::callback_boundary

/// Install a global callback-failure sink. Pass `nullptr` to revert to the
/// stderr fallback. Returns the previously installed sink (nullptr if none).
inline CallbackFailureSink set_callback_failure_sink(CallbackFailureSink sink) noexcept {
    return detail::callback_boundary::sink_storage().exchange(sink, std::memory_order_acq_rel);
}

/// Read the currently installed sink. Returns `nullptr` if the stderr
/// fallback is active. Mostly for diagnostics / introspection.
[[nodiscard]] inline CallbackFailureSink current_callback_failure_sink() noexcept {
    return detail::callback_boundary::sink_storage().load(std::memory_order_acquire);
}

/// Report a callback failure. Always succeeds (never throws). The reporter
/// will:
///   1. Try the installed sink first (if any).
///   2. If the sink throws, swallow it and fall through to the default.
///   3. Otherwise route to the stderr fallback.
inline void report_callback_failure(std::string_view   category,
                                    std::exception_ptr eptr,
                                    std::string_view   message = {}) noexcept {
    CallbackFailure f{category, std::move(eptr), message};
    if (auto* sink = detail::callback_boundary::sink_storage().load(std::memory_order_acquire);
        sink != nullptr) {
        try {
            sink(f);
            return;
        } catch (...) {
            // Sink itself threw — fall through.
        }
    }
    detail::callback_boundary::default_sink_(f);
}

/// Convenience overload taking a precomputed message and no exception
/// pointer (e.g. when the framework observed a contract violation it does
/// not wish to model as an exception).
inline void report_callback_failure(std::string_view category,
                                    std::string_view message) noexcept {
    report_callback_failure(category, std::exception_ptr{}, message);
}

}  // namespace aria
