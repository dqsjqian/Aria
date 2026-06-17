#pragma once

// Async-layer error sink — destination for exceptions that escape a
// fire-and-forget path (AsyncCommand::execute, CoroutineScope::launch,
// detached Tasks, etc.).
//
// Architecture
// ------------
// As of the unified callback-boundary work this header is now a thin
// adapter over `aria::report_callback_failure` (declared in
// `<aria/callback_boundary.hpp>`). Existing call surfaces are preserved
// exactly:
//
//   * `aria::async::set_error_sink(sink)` — installs a per-async sink
//     accepting `std::string_view`. Hosts that already used this surface
//     (notably AsyncCommand consumers and the test suite) keep working
//     unchanged.
//   * `aria::async::report_async_error(msg)` — surfaces a detached-path
//     failure. **Both** the per-async sink (if installed) and the
//     framework-wide core sink (`aria::report_callback_failure`) receive
//     a notification carrying the category `"async"`. This dual-fire
//     design ensures host applications that install ONLY the core sink
//     (the recommended modern path) still observe async failures, while
//     legacy hosts that install ONLY the async sink keep their existing
//     observability.
//   * The async sink is invoked first (legacy ordering); if it throws,
//     the exception is locally swallowed and the core sink still fires.
//
// Usage (modern):
//
//   aria::set_callback_failure_sink(&my_logger_bridge);
//   // — async failures surface as category="async".
//
// Usage (legacy / per-component):
//
//   aria::async::set_error_sink([](std::string_view m) {
//       Logger::error("aria.async", m);
//   });

#include "aria/callback_boundary.hpp"

#include <functional>
#include <string_view>
#include <utility>

namespace aria::async {

using ErrorSink = std::function<void(std::string_view)>;

inline ErrorSink& error_sink_() {
    static ErrorSink instance;
    return instance;
}

inline void set_error_sink(ErrorSink sink) noexcept {
    error_sink_() = std::move(sink);
}

inline void report_async_error(std::string_view msg) noexcept {
    // Step 1: legacy per-async sink (if installed). Local catch — async
    // sinks predate the framework-wide noexcept contract and are allowed
    // to throw.
    if (auto& s = error_sink_(); s) {
        try { s(msg); } catch (...) { /* sink must not propagate */ }
    }
    // Step 2: framework-wide unified channel. Carries an empty
    // exception_ptr because the historical async surface is
    // string-based; the message itself encodes the failure.
    ::aria::report_callback_failure(std::string_view{"async"}, msg);
}

}  // namespace aria::async
