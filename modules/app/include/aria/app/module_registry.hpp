#pragma once

#include <aria/app/extension_registry.hpp>

#include <string>

namespace aria::app {

struct ModuleDescriptor {
    std::string id;
    int order{0};
};

/// The module type is supplied by the application. Modules need not own a page,
/// depend on a particular service hub, or implement a framework virtual class.
template<class Module>
class ModuleRegistry {
    struct Entry {
        ModuleDescriptor descriptor;
        std::shared_ptr<Module> module;
    };
    ExtensionRegistry<std::string, Entry> entries_;
public:
    [[nodiscard]] aria::Subscription add(ModuleDescriptor descriptor, std::shared_ptr<Module> module) {
        if (descriptor.id.empty() || !module) throw std::invalid_argument("invalid module registration");
        auto id = descriptor.id;
        return entries_.add(std::move(id), Entry{std::move(descriptor), std::move(module)});
    }
    [[nodiscard]] std::shared_ptr<Module> find(const std::string& id) const {
        auto entry = entries_.find(id);
        return entry ? entry->module : nullptr;
    }
    [[nodiscard]] std::vector<std::shared_ptr<Module>> all() const {
        std::vector<std::shared_ptr<Module>> result;
        for (const auto& [id, entry] : entries_.snapshot()) result.push_back(entry.module);
        return result;
    }
    [[nodiscard]] std::vector<std::shared_ptr<Module>> ordered() const {
        auto entries = entries_.snapshot();
        std::stable_sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
            return left.second.descriptor.order < right.second.descriptor.order;
        });
        std::vector<std::shared_ptr<Module>> result;
        for (const auto& [id, entry] : entries) result.push_back(entry.module);
        return result;
    }
    void clear() { entries_.clear(); }
};

/// Transactional startup with reverse-order rollback and shutdown. Each start
/// step returns its cleanup handle; a failing step cleans its own partial work.
class ModuleLifecycle {
public:
    using Start = std::function<aria::Subscription()>;
    ModuleLifecycle() = default;
    ModuleLifecycle(const ModuleLifecycle&) = delete;
    ModuleLifecycle& operator=(const ModuleLifecycle&) = delete;
    ~ModuleLifecycle() { stop(); }

    void start(const std::vector<Start>& steps) {
        if (starting_ || stopping_) throw std::logic_error("recursive module lifecycle transition");
        if (started_) return;
        starting_ = true;
        cancelled_ = false;
        aria::SubscriptionBag pending;
        try {
            for (const auto& step : steps) {
                pending += step();
                if (cancelled_) throw std::runtime_error("module startup cancelled");
            }
            subscriptions_ = std::move(pending);
            started_ = true;
            starting_ = false;
        } catch (...) {
            pending.clear();
            starting_ = false;
            throw;
        }
    }
    void stop() noexcept {
        if (starting_) { cancelled_ = true; return; }
        if (stopping_) return;
        stopping_ = true;
        started_ = false;
        subscriptions_.clear();
        stopping_ = false;
    }
    [[nodiscard]] bool started() const noexcept { return started_; }
private:
    aria::SubscriptionBag subscriptions_;
    bool starting_{false};
    bool stopping_{false};
    bool started_{false};
    bool cancelled_{false};
};

} // namespace aria::app
