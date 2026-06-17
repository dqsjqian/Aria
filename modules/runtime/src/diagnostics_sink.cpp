// ============================================================================
//  diagnostics_sink.cpp
// ----------------------------------------------------------------------------
//  Shared global trace sink storage for the unified diagnostics protocol.
//
//  Why this lives in aria_runtime (a SHARED library):
//  ---------------------------------------------------
//  The header-only `inline static` approach works fine for static builds,
//  but on Windows every DLL gets its own copy of inline variables.  That
//  means a test executable installing a ScopedTraceSink only affects the
//  exe's copy, while code inside aria_binding.dll reads a separate empty
//  copy and sees "no sink installed".
//
//  By placing the actual storage in one shared module (aria_runtime) and
//  exporting it with ARIA_CORE_API, all consumers — exe and DLLs alike —
//  reference the same physical variables.
// ============================================================================

#include "aria/diagnostics.hpp"

namespace aria::detail {

std::shared_ptr<TraceSink>& global_sink_storage_() noexcept {
    static std::shared_ptr<TraceSink> sink;
    return sink;
}

std::mutex& global_sink_mutex_() noexcept {
    static std::mutex m;
    return m;
}

} // namespace aria::detail
