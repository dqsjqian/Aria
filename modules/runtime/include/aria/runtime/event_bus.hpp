#pragma once

#include "aria/abi/export.hpp"
#include "aria/detail/typed_signal.hpp"
#include "aria/subscription.hpp"
#include "aria/runtime/dispatcher.hpp"

#include <any>
#include <functional>
#include <memory>
#include <typeindex>

namespace aria::runtime {

/// Type-erased event bus. Subscribe to any type T, publish any T.
/// Singleton accessor returns the same instance process-wide because the
/// definition lives in the runtime shared library.
class ARIA_RUNTIME_API EventBus {
public:
    EventBus();
    ~EventBus();

    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;
    EventBus(EventBus&&) = delete;
    EventBus& operator=(EventBus&&) = delete;

    /// Process-wide global event bus.
    static EventBus& global() noexcept;

    template<typename T>
    void publish(const T& event) {
        std::shared_ptr<::aria::detail::TypedSignal<T>> sig;
        auto found = do_find_signal_(typeid(T));
        if (!found.has_value()) return;
        sig = std::any_cast<std::shared_ptr<::aria::detail::TypedSignal<T>>>(found);
        sig->emit(event);
    }

    template<typename T>
    [[nodiscard]] ::aria::Subscription subscribe(std::function<void(const T&)> handler) {
        std::shared_ptr<::aria::detail::TypedSignal<T>> sig;
        auto found = do_find_signal_(typeid(T));
        if (found.has_value()) {
            sig = std::any_cast<std::shared_ptr<::aria::detail::TypedSignal<T>>>(found);
        } else {
            sig = std::make_shared<::aria::detail::TypedSignal<T>>();
            auto existing = do_try_emplace_signal_(typeid(T), std::any(sig));
            if (existing.has_value()) {
                sig = std::any_cast<std::shared_ptr<::aria::detail::TypedSignal<T>>>(existing);
            }
        }
        return sig->connect(std::move(handler));
    }

    /// Subscribe with a dispatcher — incoming events are marshalled to the
    /// dispatcher's thread (e.g. the UI thread) before the handler fires.
    /// Use this if your handler touches UI and publishers may call you from
    /// a worker thread.
    template<typename T>
    [[nodiscard]] ::aria::Subscription subscribe_on(
        IDispatcher& dispatcher,
        std::function<void(const T&)> handler)
    {
        auto shared_handler = std::make_shared<std::function<void(const T&)>>(
            std::move(handler));
        return subscribe<T>([&dispatcher, shared_handler](const T& ev) {
            // Copy the event into a shared_ptr so the posted lambda can
            // safely outlive the caller.
            auto keep = std::make_shared<T>(ev);
            dispatcher.post([shared_handler, keep]() {
                (*shared_handler)(*keep);
            });
        });
    }

    void clear();

private:
    std::any do_find_signal_(std::type_index ti);
    std::any do_try_emplace_signal_(std::type_index ti, std::any sig);

    struct Impl;
    // RAII pImpl. MSVC would emit C4251 for a std::unique_ptr member of a
    // dll-exported class (the unique_ptr template is not itself exported),
    // but the pointee is an *incomplete* opaque type consumed only through
    // this module's non-template API, so the warning is a false positive —
    // suppress it locally rather than hand-managing a raw pointer.
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
