// Shared diagnostic storage belongs to the compiled foundation. Core-only
// consumers and every shared adapter therefore reach the same physical sink.

#include "aria/diagnostics.hpp"
#include "aria/callback_boundary.hpp"

namespace aria::detail {

std::shared_ptr<TraceSink>& global_sink_storage_() noexcept {
    static std::shared_ptr<TraceSink> sink;
    return sink;
}

std::mutex& global_sink_mutex_() noexcept {
    static std::mutex m;
    return m;
}

std::atomic<bool>& trace_sink_present_() noexcept {
    static std::atomic<bool> present{false};
    return present;
}

void dispatch_trace_(const std::shared_ptr<TraceSink>& sink,
                     const TraceEvent& event) noexcept {
    static thread_local bool dispatching = false;
    if (dispatching) return;
    struct Guard {
        bool& active;
        explicit Guard(bool& value) : active(value) { active = true; }
        ~Guard() { active = false; }
    } guard{dispatching};
    try {
        (*sink)(event);
    } catch (...) {
        aria::report_callback_failure("diagnostics.trace_sink", std::current_exception());
    }
}

} // namespace aria::detail
