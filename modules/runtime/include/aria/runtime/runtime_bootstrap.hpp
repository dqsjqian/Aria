#pragma once

// runtime_bootstrap — host-application entry point for wiring framework
// diagnostics together.
//
// What this does
// --------------
// `aria::runtime::install_default_diagnostics()` performs two pieces of
// startup-time wiring:
//
//   1. Installs an `aria::CallbackFailureSink` that routes every framework-
//      internal callback failure (executor worker / main-thread drain /
//      simple dispatcher pump / virtual-time advance / async detached
//      tasks / abi slot trampoline) into `aria::runtime::Logger::error`,
//      tagged with the failure's category as the log category.
//
//   2. Installs an `aria::abi::SlotInvokeFailureHook` that forwards
//      exceptions escaping the ABI slot trampoline to the same callback-
//      boundary channel — preserving layering (abi does not depend on
//      core) while making slot failures observable end-to-end.
//
// Calling pattern
// ---------------
//   int main(int argc, char** argv) {
//       aria::runtime::install_default_diagnostics();
//       // ... rest of process startup ...
//   }
//
// Idempotence
// -----------
// The function is idempotent: calling it twice replaces the sink/hook in
// place without leaking previous installations. `uninstall_default_
// diagnostics()` reverts both wirings to whatever was in place before the
// most recent install (typically `nullptr`, i.e. the stderr fallback).
//
// Threading
// ---------
// Both APIs are thread-safe but are intended to be called once from the
// process's bootstrap thread before any framework component fires.

#include "aria/abi/export.hpp"

namespace aria::runtime {

/// Install the default diagnostics wiring (callback-boundary sink → Logger,
/// abi slot hook → callback boundary). Returns true if a wiring became
/// active; returns false if it was already active and nothing changed.
ARIA_RUNTIME_API bool install_default_diagnostics() noexcept;

/// Revert both wirings to whatever they were before the most recent
/// `install_default_diagnostics()` call. Safe to invoke when no
/// installation is currently active (no-op).
ARIA_RUNTIME_API void uninstall_default_diagnostics() noexcept;

}  // namespace aria::runtime
