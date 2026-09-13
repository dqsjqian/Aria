#include "aria/runtime/container.hpp"

#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aria::runtime {

struct Container::Impl {
    mutable std::mutex mutex;
    std::unordered_map<std::type_index, std::shared_ptr<const Registration>> registrations;
    std::vector<std::type_index> order;
};

Container::Container() : impl_(std::make_shared<Impl>()) {}

Container::~Container() {
    clear();
}

void Container::clear() {
    // Keep the registry alive if a released service destroys the container.
    const auto state = impl_;
    for (;;) {
        std::shared_ptr<const Registration> doomed;
        {
            std::lock_guard lock(state->mutex);
            if (state->order.empty()) return;
            const auto type = state->order.back();
            state->order.pop_back();
            const auto it = state->registrations.find(type);
            doomed = std::move(it->second);
            state->registrations.erase(it);
        }
        // Release one registration at a time, outside the lock. Earlier
        // providers remain available to destructors that resolve dependencies.
    }
}

void Container::do_register_(std::type_index type, std::any payload, bool factory) {
    auto replacement = std::make_shared<const Registration>(
        Registration{std::move(payload), factory});
    const auto state = impl_;
    {
        std::lock_guard lock(state->mutex);
        if (auto it = state->registrations.find(type); it != state->registrations.end()) {
            it->second.swap(replacement);
        } else {
            state->order.push_back(type);
            try {
                // Retain our copy until after unlocking, including on failure:
                // an allocator exception must not destroy user captures here.
                state->registrations.emplace(type, replacement);
            } catch (...) {
                state->order.pop_back();
                throw;
            }
        }
    }
}

std::shared_ptr<const Container::Registration>
Container::do_find_(std::type_index type) const {
    std::lock_guard lock(impl_->mutex);
    const auto it = impl_->registrations.find(type);
    return it == impl_->registrations.end() ? nullptr : it->second;
}

bool Container::do_has_(std::type_index type) const {
    std::lock_guard lock(impl_->mutex);
    return impl_->registrations.contains(type);
}

}  // namespace aria::runtime
