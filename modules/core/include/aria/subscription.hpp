#pragma once

#include "aria/callback_boundary.hpp"

// ============================================================================
//  subscription.hpp
// ----------------------------------------------------------------------------
//  Unified RAII subscription handle for the whole framework.
//
//  In Aria there are two independent event mechanisms:
//
//    1. reactive::Graph -- dependency-tracked values and derivations
//       (Property, Computed, Effect). Its subscriptions own a
//       Reaction node; dropping the handle destroys the node, which
//       automatically detaches from the graph.
//
//    2. abi::SignalErased -- type-erased, fire-and-forget signals used
//       for events that do not participate in dependency tracking
//       (ObservableList diffs, Command::can_execute_changed, EventBus).
//       Its subscriptions own a small "disconnector" shim whose
//       destructor calls back into the signal to remove the slot.
//
//  Both live behind a single user-facing handle: `Subscription`. Internally
//  the handle is nothing more than a `std::shared_ptr<void>` -- destruction
//  of the last-owning Subscription destroys whatever the void* points at,
//  which in turn performs the backend-specific disconnect.
//
//  This unification is what lets `SubscriptionBag` aggregate any mix of
//  reactive/event subscriptions without caring about their origin.
// ============================================================================

#include <cstddef>
#include <concepts>
#include <type_traits>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace aria {

/// RAII handle to a single subscription. Dropping it (or calling
/// `release()`) severs the underlying connection. Move-only.
class Subscription {
public:
    Subscription() noexcept = default;

    /// Construct from any shared pointer. The common case: pass a
    /// `std::shared_ptr<ReactionNode>` returned from the reactive layer;
    /// its destructor detaches from the graph.
    template<class T>
    explicit Subscription(std::shared_ptr<T> owner) noexcept
        : owner_(std::move(owner)) {}

    /// Invoke a disconnect callback exactly once when this handle is released.
    /// Captures are stored directly in the shared allocation, including
    /// move-only captures; there is no intermediate std::function allocation.
    template<class Fn>
        requires std::invocable<std::decay_t<Fn>&>
    explicit Subscription(Fn&& on_disconnect) {
        if constexpr (std::is_pointer_v<std::decay_t<Fn>>) {
            if (!on_disconnect) return;
        }
        owner_ = std::make_shared<CallbackDeleter<std::decay_t<Fn>>>(
            std::forward<Fn>(on_disconnect));
    }

    explicit Subscription(std::function<void()> on_disconnect) {
        if (on_disconnect) {
            owner_ = std::make_shared<CallbackDeleter<std::function<void()>>>(
                std::move(on_disconnect));
        }
    }

    Subscription(const Subscription&)            = delete;
    Subscription& operator=(const Subscription&) = delete;
    Subscription(Subscription&&) noexcept                 = default;
    Subscription& operator=(Subscription&&) noexcept      = default;

    ~Subscription() noexcept = default;

    /// Explicitly disconnect now (instead of at destruction).
    void release() noexcept { owner_.reset(); }

    [[nodiscard]] bool active() const noexcept { return static_cast<bool>(owner_); }
    explicit operator bool() const noexcept { return active(); }

private:
    template<class Fn>
    struct CallbackDeleter {
        Fn fn;

        template<class F>
        explicit CallbackDeleter(F&& callback) : fn(std::forward<F>(callback)) {}

        CallbackDeleter(CallbackDeleter&&) = delete;
        CallbackDeleter& operator=(CallbackDeleter&&) = delete;
        CallbackDeleter(const CallbackDeleter&) = delete;
        CallbackDeleter& operator=(const CallbackDeleter&) = delete;

        ~CallbackDeleter() noexcept {
            try {
                std::invoke(fn);
            } catch (...) {
                report_callback_failure("subscription.disconnect", std::current_exception());
            }
        }
    };

    std::shared_ptr<void> owner_;
};

/// Aggregate holder: owns multiple Subscriptions and drops them together.
/// Typical use: a ViewModel keeps a bag and `+=` every subscription it
/// opens; destroying the VM tears everything down in one step.
class SubscriptionBag {
public:
    SubscriptionBag() = default;

    SubscriptionBag(const SubscriptionBag&)            = delete;
    SubscriptionBag& operator=(const SubscriptionBag&) = delete;
    SubscriptionBag(SubscriptionBag&& other) noexcept : subs_(std::move(other.subs_)) {}
    SubscriptionBag& operator=(SubscriptionBag&& other) noexcept {
        if (this != &other) {
            auto previous = std::move(subs_);
            subs_ = std::move(other.subs_);
            release_reverse_(previous);
        }
        return *this;
    }

    ~SubscriptionBag() noexcept {
        destroying_ = true;
        clear();
    }

    void add(Subscription s) {
        if (!destroying_) subs_.push_back(std::move(s));
    }

    SubscriptionBag& operator+=(Subscription s) {
        add(std::move(s));
        return *this;
    }

    /// Disconnect the current contents in reverse insertion order.
    /// Reentrant additions belong to the new bag and survive this clear.
    void clear() noexcept {
        std::vector<Subscription> previous;
        previous.swap(subs_);
        release_reverse_(previous);
    }

    [[nodiscard]] std::size_t size() const noexcept { return subs_.size(); }
    [[nodiscard]] bool        empty() const noexcept { return subs_.empty(); }

private:
    static void release_reverse_(std::vector<Subscription>& subscriptions) noexcept {
        while (!subscriptions.empty()) {
            auto last = std::move(subscriptions.back());
            subscriptions.pop_back();
            last.release();
        }
    }

    std::vector<Subscription> subs_;
    bool destroying_ = false;
};

}  // namespace aria
