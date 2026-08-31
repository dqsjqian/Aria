#include "aria/runtime/container.hpp"
#include <memory>

#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aria::runtime {

struct Container::Impl {
    /// One registration slot, in the order it was registered. `singleton`
    /// selects which table owns the value, so a type registered in both
    /// modes keeps two independent slots.
    struct Entry {
        std::type_index ti;
        bool            singleton;
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::type_index, std::any> singletons_;
    std::unordered_map<std::type_index, std::any> typed_factories_;
    /// Registration order. Teardown walks this back-to-front (L-40).
    std::vector<Entry> order_;
};

Container::Container() : impl_(std::make_unique<Impl>()) {}

Container::~Container() {
    // Reverse-order teardown is part of the contract, so the destructor
    // must not fall back to unordered_map's unspecified clear().
    clear();
}

void Container::clear() {
    // Pop one entry at a time and let it die with the mutex released.
    //
    // Two properties depend on this shape and neither survives a
    // "move everything out, then destroy" rewrite:
    //   * a service destructor that re-enters the container (resolve /
    //     has / register) does not deadlock;
    //   * while entry N is being destroyed, entries 1..N-1 are still in
    //     the tables, so a consumer can still reach a provider it was
    //     registered after.
    for (;;) {
        std::any doomed;
        {
            std::lock_guard lk(impl_->mutex_);
            if (impl_->order_.empty()) {
                // Defensive: the public API cannot produce a table entry
                // without an order entry, but never leak one either.
                impl_->singletons_.clear();
                impl_->typed_factories_.clear();
                return;
            }
            const Impl::Entry entry = impl_->order_.back();
            impl_->order_.pop_back();
            auto& table = entry.singleton ? impl_->singletons_
                                          : impl_->typed_factories_;
            if (auto it = table.find(entry.ti); it != table.end()) {
                doomed = std::move(it->second);
                table.erase(it);
            }
        }
        // `doomed` is destroyed here, outside the lock.
    }
}

void Container::do_register_instance_(std::type_index ti, std::any ptr) {
    std::any replaced;
    {
        std::lock_guard lk(impl_->mutex_);
        if (auto it = impl_->singletons_.find(ti); it != impl_->singletons_.end()) {
            // Re-registering keeps the original teardown position: the
            // type's place in the dependency order did not change, only
            // the instance behind it.
            replaced = std::move(it->second);
            it->second = std::move(ptr);
        } else {
            impl_->singletons_.emplace(ti, std::move(ptr));
            impl_->order_.push_back(Impl::Entry{ti, true});
        }
    }
    // `replaced` dies outside the lock, same rule as clear().
}

void Container::do_register_transient_(std::type_index ti, std::any fn) {
    std::any replaced;
    {
        std::lock_guard lk(impl_->mutex_);
        if (auto it = impl_->typed_factories_.find(ti);
            it != impl_->typed_factories_.end()) {
            replaced = std::move(it->second);
            it->second = std::move(fn);
        } else {
            impl_->typed_factories_.emplace(ti, std::move(fn));
            impl_->order_.push_back(Impl::Entry{ti, false});
        }
    }
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
