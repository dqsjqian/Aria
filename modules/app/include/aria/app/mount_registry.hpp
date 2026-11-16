#pragma once

#include <aria/app/extension_registry.hpp>
#include <aria/binding/view_model.hpp>

#include <string>
#include <typeindex>

namespace aria::app {

struct Mounted {
    std::string provider_id;
    std::shared_ptr<aria::binding::ViewModel> vm;
};

template<class Context>
class MountRegistry {
public:
    using Factory = std::function<std::shared_ptr<aria::binding::ViewModel>(Context&)>;
    explicit MountRegistry(Context& context) : context_(context) {}
    [[nodiscard]] aria::Subscription provide(std::string slot, std::string provider_id, Factory factory) {
        if (slot.empty() || provider_id.empty() || !factory)
            throw std::invalid_argument("invalid mount registration");
        return entries_.add(std::move(slot), Entry{std::move(provider_id), std::move(factory)});
    }
    [[nodiscard]] std::optional<Mounted> resolve(const std::string& slot) {
        auto entry = entries_.find(slot);
        if (!entry) return std::nullopt;
        auto vm = entry->factory(context_);
        if (!vm) return std::nullopt;
        return Mounted{std::move(entry->provider_id), std::move(vm)};
    }
    template<class Interface, class Payload>
    [[nodiscard]] std::optional<Mounted> resolve(const std::string& slot, const Payload& payload) {
        auto mounted = resolve(slot);
        if (!mounted) return std::nullopt;
        auto* target = dynamic_cast<Interface*>(mounted->vm.get());
        if (!target || !target->on_mount(payload)) return std::nullopt;
        return mounted;
    }
    [[nodiscard]] std::optional<std::string> module_of(const std::string& slot) const {
        auto entry = entries_.find(slot);
        return entry ? std::optional<std::string>{entry->provider_id} : std::nullopt;
    }
    bool set_enabled(const std::string& slot, bool enabled) { return entries_.set_enabled(slot, enabled); }
    bool remove(const std::string& slot) { return entries_.remove(slot); }
    [[nodiscard]] bool provided(const std::string& slot) const { return entries_.contains(slot); }
    [[nodiscard]] aria::Subscription on_changed(std::function<void()> callback) {
        return entries_.on_changed(std::move(callback));
    }
    void clear() { entries_.clear(); }
private:
    struct Entry { std::string provider_id; Factory factory; };
    Context& context_;
    ExtensionRegistry<std::string, Entry> entries_;
};

/// Typed page contributions. Resolving creates and validates a target; callers
/// push it through binding::Navigator with their own presentation metadata.
/// No JSON library, platform widget or business interface is required here.
template<class Context>
class TypedRouteRegistry {
public:
    using Factory = typename MountRegistry<Context>::Factory;
    explicit TypedRouteRegistry(Context& context) : context_(context) {}
    template<class Interface>
    [[nodiscard]] aria::Subscription add(std::string provider_id, Factory factory) {
        if (provider_id.empty() || !factory) throw std::invalid_argument("invalid route registration");
        return entries_.add(typeid(Interface), Entry{std::move(provider_id), std::move(factory)});
    }
    template<class Interface, class Payload>
    [[nodiscard]] std::optional<Mounted> resolve(const Payload& payload) {
        auto entry = entries_.find(typeid(Interface));
        if (!entry) return std::nullopt;
        auto vm = entry->factory(context_);
        auto* target = dynamic_cast<Interface*>(vm.get());
        if (!target || !target->on_navigate(payload)) return std::nullopt;
        return Mounted{std::move(entry->provider_id), std::move(vm)};
    }
    template<class Interface>
    bool remove() { return entries_.remove(typeid(Interface)); }
    void clear() { entries_.clear(); }
private:
    struct Entry { std::string provider_id; Factory factory; };
    Context& context_;
    ExtensionRegistry<std::type_index, Entry> entries_;
};

} // namespace aria::app
