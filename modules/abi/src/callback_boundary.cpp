// ============================================================================
//  callback_boundary.cpp (aria_abi)
// ----------------------------------------------------------------------------
//  Single-TU storage for the framework-wide callback-boundary primitives:
//
//    * `aria::detail::callback_boundary::sink_storage()` — global sink slot.
//    * `aria::abi::detail::slot_invoke_failure_hook()`   — global slot hook.
//
//  Why this lives in libaria_abi:
//  ------------------------------
//  `aria::callback_boundary` and `aria::abi::SlotInvokeFailureHook` need a
//  *single* atomic-pointer slot reachable from every consuming module —
//  the host exe, libaria_runtime.dylib, libaria_binding.dylib, etc.
//  Inline static variables in headers would give each SHARED library its
//  own copy; tests installing a sink in the exe would never be observed
//  by code inside aria_runtime.
//
//  How the single-slot invariant is currently maintained
//  -----------------------------------------------------
//  The two accessors below are exported with `ARIA_ABI_API`. They are
//  defined in this single TU and compiled into `libaria_abi`. In the
//  shipped layout `libaria_abi` is a static archive that is linked
//  exclusively into `libaria_runtime` (with PIC). Every other shared
//  consumer — `libaria_binding`, the platform adapters, the host exe —
//  links against `libaria_runtime` and resolves these symbols through
//  it. That is what produces the single physical slot per process.
//
//  IMPORTANT: this guarantee does NOT come from "static archive linked
//  into every shared consumer". If a future build configuration linked
//  `libaria_abi` directly into more than one shared module, each such
//  module would carry its own copy of these statics and the invariant
//  would silently break (sinks installed in the exe would not reach
//  callbacks raised inside that other shared module). The CMake setup
//  enforces "abi → runtime only" for that reason; if you change the
//  link graph, add a CI symbol-uniqueness check on `sink_storage` and
//  `slot_invoke_failure_hook`, or move ownership of these statics into
//  a single shared library (e.g. `aria_runtime`) and have `aria_abi`
//  expose them only as undefined references.
// ============================================================================

#include "aria/abi/slot_factory.hpp"
#include "aria/callback_boundary.hpp"

namespace aria::detail::callback_boundary {

std::atomic<CallbackFailureSink>& sink_storage() noexcept {
    static std::atomic<CallbackFailureSink> sink{nullptr};
    return sink;
}

}  // namespace aria::detail::callback_boundary

namespace aria::abi::detail {

std::atomic<SlotInvokeFailureHook>& slot_invoke_failure_hook() noexcept {
    static std::atomic<SlotInvokeFailureHook> hook{nullptr};
    return hook;
}

}  // namespace aria::abi::detail
