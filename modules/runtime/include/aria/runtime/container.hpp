#pragma once

#include "aria/abi/export.hpp"
#include <any>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <typeindex>

namespace aria::runtime {

/// Type-keyed dependency-injection container. Thread-safe.
///
/// Supports three registration modes:
///   - register_singleton<I, Impl>()   : one shared instance for life of container
///   - register_transient<I, Impl>()   : new instance every resolve()
///   - register_factory<I>(fn)         : custom factory function
///   - register_instance<I>(ptr)       : pre-built singleton
class ARIA_RUNTIME_API Container {
public:
    Container();
    ~Container();

    Container(const Container&) = delete;
    Container& operator=(const Container&) = delete;
    Container(Container&&) = delete;
    Container& operator=(Container&&) = delete;

    template<typename Interface, typename Impl>
    void register_singleton() {
        std::shared_ptr<Interface> ptr = std::make_shared<Impl>();
        register_instance_<Interface>(std::move(ptr));
    }

    template<typename Interface>
    void register_instance(std::shared_ptr<Interface> ptr) {
        register_instance_<Interface>(std::move(ptr));
    }

    template<typename Interface, typename Impl>
    void register_transient() {
        std::function<std::shared_ptr<Interface>()> typed =
            []() -> std::shared_ptr<Interface> { return std::make_shared<Impl>(); };
        do_register_transient_(typeid(Interface), std::any(std::move(typed)));
    }

    template<typename Interface>
    void register_factory(std::function<std::shared_ptr<Interface>()> fn) {
        do_register_transient_(typeid(Interface), std::any(std::move(fn)));
    }

    template<typename Interface>
    [[nodiscard]] std::shared_ptr<Interface> resolve() {
        auto found = do_find_(typeid(Interface));
        if (found.singleton.has_value()) {
            return std::any_cast<std::shared_ptr<Interface>>(found.singleton);
        }
        if (found.factory.has_value()) {
            auto fn = std::any_cast<std::function<std::shared_ptr<Interface>()>>(found.factory);
            return fn();
        }
        throw std::runtime_error(std::string("Container: not registered: ")
                                 + typeid(Interface).name());
    }

    template<typename Interface>
    [[nodiscard]] bool has() const {
        return do_has_(typeid(Interface));
    }

    void clear();

private:
    template<typename Interface>
    void register_instance_(std::shared_ptr<Interface> ptr) {
        do_register_instance_(typeid(Interface), std::any(std::move(ptr)));
    }

    struct FoundPair {
        std::any singleton;
        std::any factory;
    };

    void do_register_instance_(std::type_index ti, std::any ptr);
    void do_register_transient_(std::type_index ti, std::any fn);
    FoundPair do_find_(std::type_index ti) const;
    bool do_has_(std::type_index ti) const;

    struct Impl;
    // RAII pImpl; C4251 on the unique_ptr member is a false positive for an
    // incomplete opaque pointee consumed only via non-template API.
#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif
    std::unique_ptr<Impl> impl_;
#ifdef _MSC_VER
#  pragma warning(pop)
#endif
};

}  // namespace aria::runtime
