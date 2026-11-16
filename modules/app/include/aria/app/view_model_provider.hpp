#pragma once

#include <aria/app/extension_registry.hpp>
#include <aria/binding/view_model.hpp>

#include <string>
#include <unordered_set>

namespace aria::app {

/// Factories and cached primary instances are separate from fresh navigation
/// instances. Context must outlive the provider. All access is owner-thread only.
template<class Context>
class ViewModelProvider {
public:
    using ViewModel = aria::binding::ViewModel;
    using Factory = std::function<std::shared_ptr<ViewModel>(Context&)>;
    explicit ViewModelProvider(Context& context) : context_(context) {
        changes_ = factories_.on_changed([this] {
            // Remove only invalidated entries: unrelated registrations preserve
            // live primary VMs and their application state.
            // First detach all invalidated nodes, then destroy their VMs.
            // VM destructors may reenter the provider and rehash cache_.
            std::vector<typename decltype(cache_)::node_type> removed;
            removed.reserve(cache_.size());
            for (auto it = cache_.begin(); it != cache_.end();) {
                if (factories_.generation(it->first) != it->second.generation) {
                    removed.push_back(cache_.extract(it++));
                } else {
                    ++it;
                }
            }
        });
    }
    ~ViewModelProvider() {
        // A cached VM or factory capture may call back into this provider while
        // being destroyed. Run that destruction while every member is alive,
        // with new work disabled and cache-change callbacks disconnected.
        shutting_down_ = true;
        changes_.release();
        clear_cache();
        factories_.clear();
    }
    [[nodiscard]] aria::Subscription add(std::string id, Factory factory) {
        if (shutting_down_) throw std::logic_error("view model provider is shutting down");
        if (id.empty() || !factory) throw std::invalid_argument("invalid view model factory");
        return factories_.add(std::move(id), std::move(factory));
    }
    [[nodiscard]] std::shared_ptr<ViewModel> create(const std::string& id) {
        if (shutting_down_) return nullptr;
        auto factory = factories_.find(id);
        if (!factory) return nullptr;
        if (!constructing_.insert(id).second) throw std::logic_error("recursive view model construction: " + id);
        try {
            auto result = (*factory)(context_);
            constructing_.erase(id);
            return result;
        } catch (...) {
            constructing_.erase(id);
            throw;
        }
    }
    [[nodiscard]] std::shared_ptr<ViewModel> get(const std::string& id) {
        if (shutting_down_) return nullptr;
        if (auto cached = cache_.find(id); cached != cache_.end()) return cached->second.vm;
        const auto generation = factories_.generation(id);
        auto vm = create(id);
        if (vm && generation && factories_.generation(id) == generation) {
            cache_.emplace(id, Cached{generation, vm});
        }
        return vm;
    }
    template<class T>
    [[nodiscard]] std::shared_ptr<T> get_as(const std::string& id) {
        return std::dynamic_pointer_cast<T>(get(id));
    }
    void clear_cache() {
        // Reentrant cache additions belong to the new cache and survive.
        decltype(cache_) removed;
        removed.swap(cache_);
    }
private:
    struct Cached { std::uint64_t generation; std::shared_ptr<ViewModel> vm; };
    Context& context_;
    ExtensionRegistry<std::string, Factory> factories_;
    std::unordered_map<std::string, Cached> cache_;
    std::unordered_set<std::string> constructing_;
    aria::Subscription changes_;
    bool shutting_down_{false};
};

} // namespace aria::app
