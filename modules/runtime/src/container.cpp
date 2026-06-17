#include "aria/runtime/container.hpp"
#include <memory>

#include <mutex>
#include <unordered_map>

namespace aria::runtime {

struct Container::Impl {
    mutable std::mutex mutex_;
    std::unordered_map<std::type_index, std::any> singletons_;
    std::unordered_map<std::type_index, std::any> typed_factories_;
};

Container::Container() : impl_(std::make_unique<Impl>()) {}
Container::~Container() = default;

void Container::clear() {
    std::lock_guard lk(impl_->mutex_);
    impl_->singletons_.clear();
    impl_->typed_factories_.clear();
}

void Container::do_register_instance_(std::type_index ti, std::any ptr) {
    std::lock_guard lk(impl_->mutex_);
    impl_->singletons_[ti] = std::move(ptr);
}

void Container::do_register_transient_(std::type_index ti, std::any fn) {
    std::lock_guard lk(impl_->mutex_);
    impl_->typed_factories_[ti] = std::move(fn);
}

Container::FoundPair Container::do_find_(std::type_index ti) const {
    FoundPair result;
    std::lock_guard lk(impl_->mutex_);
    auto it_inst = impl_->singletons_.find(ti);
    if (it_inst != impl_->singletons_.end()) {
        result.singleton = it_inst->second;
    }
    auto it_fac = impl_->typed_factories_.find(ti);
    if (it_fac != impl_->typed_factories_.end()) {
        result.factory = it_fac->second;
    }
    return result;
}

bool Container::do_has_(std::type_index ti) const {
    std::lock_guard lk(impl_->mutex_);
    return impl_->singletons_.count(ti) > 0
        || impl_->typed_factories_.count(ti) > 0;
}

}  // namespace aria::runtime
