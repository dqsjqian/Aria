#pragma once

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

    /// Construct from a "disconnect" callback. Invoked exactly once, on
    /// the last Subscription's destruction. Used by `detail::TypedSignal`
    /// and other abi-signal-backed event producers.
    ///
    /// Implementation: a single `make_shared<CallbackDeleter>(...)` —
    /// one heap allocation that fuses the control block AND the
    /// callable storage. Earlier revisions did `static_pointer_cast`
    /// from a typed shared_ptr to `shared_ptr<void>`, which compiled
    /// to the same single allocation but obscured intent. We keep the
    /// deleter type alive directly via `shared_ptr<CallbackDeleter>`,
    /// erased through `shared_ptr<void>` only at the storage layer.
    explicit Subscription(std::function<void()> on_disconnect)
        : owner_(make_callback_(std::move(on_disconnect))) {}

    Subscription(const Subscription&)            = delete;
    Subscription& operator=(const Subscription&) = delete;
    Subscription(Subscription&&) noexcept                 = default;
    Subscription& operator=(Subscription&&) noexcept      = default;

    ~Subscription() noexcept = default;

    /// Transfer-away: the destructor will NOT disconnect.
    /// Equivalent to `release()` at the call-site level.
    void detach() noexcept { owner_.reset(); }

    /// Explicitly disconnect now (instead of at destruction).
    void release() noexcept { owner_.reset(); }

    [[nodiscard]] bool active() const noexcept { return static_cast<bool>(owner_); }
    explicit operator bool() const noexcept { return active(); }

private:
    // Tiny helper that runs a std::function on destruction. We hide it
    // behind shared_ptr<void> so that Subscription stays a lean,
    // backend-agnostic handle.
    //
    // Construction contract: CallbackDeleter is built exactly once, in
    // place inside shared_ptr's control block (see `make_callback_`), and
    // is destroyed exactly once when the last Subscription dies. No move
    // or copy is ever performed on it -- hence the deleted special
    // members -- so the destructor can invoke `fn` unconditionally
    // without any "was this moved-from?" guard.
    struct CallbackDeleter {
        std::function<void()> fn;

        explicit CallbackDeleter(std::function<void()> f) noexcept
            : fn(std::move(f)) {}

        CallbackDeleter(CallbackDeleter&&)                 = delete;
        CallbackDeleter& operator=(CallbackDeleter&&)      = delete;
        CallbackDeleter(const CallbackDeleter&)            = delete;
        CallbackDeleter& operator=(const CallbackDeleter&) = delete;

        ~CallbackDeleter() { if (fn) fn(); }
    };

    static std::shared_ptr<void> make_callback_(std::function<void()> fn) {
        if (!fn) return {};
        // Allocate the CallbackDeleter in-place inside the shared control
        // block. Perfect-forwarding `std::move(fn)` means no temporary
        // CallbackDeleter is ever constructed, so there is no chance of
        // a moved-from std::function firing on destruction.
        return std::static_pointer_cast<void>(
            std::make_shared<CallbackDeleter>(std::move(fn)));
    }

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
    SubscriptionBag(SubscriptionBag&&) noexcept                 = default;
    SubscriptionBag& operator=(SubscriptionBag&&) noexcept      = default;

    void add(Subscription s) { subs_.push_back(std::move(s)); }

    SubscriptionBag& operator+=(Subscription s) {
        add(std::move(s));
        return *this;
    }

    void clear() noexcept { subs_.clear(); }

    [[nodiscard]] std::size_t size() const noexcept { return subs_.size(); }
    [[nodiscard]] bool        empty() const noexcept { return subs_.empty(); }

private:
    std::vector<Subscription> subs_;
};

}  // namespace aria
