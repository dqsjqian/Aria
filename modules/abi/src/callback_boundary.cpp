// Callback-failure storage and recursion guards live in one compiled
// foundation library shared by core, runtime, binding, and platform adapters.

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

namespace aria {

void report_callback_failure(std::string_view category, std::exception_ptr exception,
                             std::string_view message) noexcept {
    // This function lives in one shared module, so recursion through another
    // module observes the same per-thread guard.
    static thread_local bool reporting = false;
    if (reporting) {
        std::fputs("[aria.callback_failure] recursive report suppressed\n", stderr);
        return;
    }
    struct Guard {
        bool& active;
        explicit Guard(bool& value) : active(value) { active = true; }
        ~Guard() { active = false; }
    } guard{reporting};
    const CallbackFailure failure{category, std::move(exception), message};
    if (const auto sink = current_callback_failure_sink()) {
        try {
            sink(failure);
            return;
        } catch (...) {
        }
    }
    detail::callback_boundary::default_sink_(failure);
}

}  // namespace aria

namespace aria::abi::detail {

void report_slot_invoke_failure_(std::exception_ptr exception) noexcept {
    static thread_local bool invoking_hook = false;
    if (!invoking_hook) {
        if (const auto hook = slot_invoke_failure_hook().load(std::memory_order_acquire)) {
            struct Guard {
                bool& active;
                explicit Guard(bool& value) : active(value) { active = true; }
                ~Guard() { active = false; }
            } guard{invoking_hook};
            try {
                hook(exception);
                return;
            } catch (...) {
            }
        }
    }
    aria::report_callback_failure("abi.slot.invoke", std::move(exception));
}

}  // namespace aria::abi::detail
