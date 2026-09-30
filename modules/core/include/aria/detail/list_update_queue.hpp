#pragma once

#include <vector>
#include <exception>
#include <functional>
#include <mutex>
#include <utility>

namespace aria::detail {

/// Serialize derived-list updates, including writes made by a predicate or
/// comparator. No queue lock spans user code. A nested/concurrent submission
/// follows the current complete update; failures cannot strand later edits.
class ListUpdateQueue {
public:
    template<class Update>
    void submit(Update update) {
        {
            std::scoped_lock lock(mutex_);
            if (running_) {
                pending_.emplace_back(std::move(update));
                return;
            }
            running_ = true;
        }
        // Keep the ordinary non-reentrant path on the caller's stack.
        std::exception_ptr first_error;
        try { update(); }
        catch (...) { first_error = std::current_exception(); }
        for (;;) {
            std::function<void()> current;
            {
                std::scoped_lock lock(mutex_);
                if (head_ == pending_.size()) {
                    pending_.clear(); // all captures have already moved out
                    head_ = 0;
                    running_ = false;
                    break;
                }
                current = std::move(pending_.at(head_++));
            }
            try { current(); }
            catch (...) { if (!first_error) { first_error = std::current_exception(); } }
        }
        if (first_error) { std::rethrow_exception(first_error); }
    }

private:
    std::mutex mutex_;
    std::vector<std::function<void()>> pending_;
    std::size_t head_ = 0;
    bool running_ = false;
};

} // namespace aria::detail
