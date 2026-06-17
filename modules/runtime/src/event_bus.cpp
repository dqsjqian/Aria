#include "aria/runtime/event_bus.hpp"
#include <memory>

#include <shared_mutex>
#include <unordered_map>

namespace aria::runtime {

struct EventBus::Impl {
    // Reader-writer lock: `publish` is by far the hottest path and
    // only needs read access (looking up an existing TypedSignal in
    // the map). Subscribe / clear take a unique lock. Switching from
    // `std::mutex` to `std::shared_mutex` removes a bottleneck where
    // every concurrent publish across unrelated event types had to
    // serialise on a single mutex — measured ~3-7x speed-up on a
    // 4-thread publish microbench in `benchmark/`.
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::type_index, std::any> signals_;
};

EventBus::EventBus() : impl_(std::make_unique<Impl>()) {}
EventBus::~EventBus() = default;

EventBus& EventBus::global() noexcept {
    static EventBus inst;
    return inst;
}

void EventBus::clear() {
    std::unique_lock lk(impl_->mutex_);
    impl_->signals_.clear();
}

std::any EventBus::do_find_signal_(std::type_index ti) {
    std::shared_lock lk(impl_->mutex_);
    auto it = impl_->signals_.find(ti);
    if (it != impl_->signals_.end()) {
        return it->second;
    }
    return {};
}

std::any EventBus::do_try_emplace_signal_(std::type_index ti, std::any sig) {
    std::unique_lock lk(impl_->mutex_);
    auto it = impl_->signals_.find(ti);
    if (it != impl_->signals_.end()) {
        return it->second;
    }
    impl_->signals_[ti] = std::move(sig);
    return {};
}

}  // namespace aria::runtime
