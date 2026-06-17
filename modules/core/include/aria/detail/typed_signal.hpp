#pragma once

// ============================================================================
//  detail/typed_signal.hpp
// ----------------------------------------------------------------------------
//  Strongly-typed wrapper around the ABI-stable `aria::abi::SignalErased`.
//
//  Unlike the reactive graph (which is for dependency-tracked values),
//  this signal primitive is for *fire-and-forget events* that cross
//  type-erasure boundaries (ObservableList diffs, Command::can_execute
//  notifications, EventBus events, the Qt adapter's property change
//  bridge, ...).
//
//  Subscriptions returned here share the single unified `Subscription`
//  handle: the slot is disconnected when the last-owning handle is
//  destroyed, via a small on-destruction callback injected into the
//  handle.
// ============================================================================

#include "aria/abi/signal.hpp"
#include "aria/abi/slot.hpp"
#include "aria/abi/slot_factory.hpp"
#include "aria/subscription.hpp"

#include <cstddef>
#include <functional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace aria::detail {

/// Strongly-typed multicast signal. Args are marshalled through a
/// `Bundle` struct so the underlying `abi::SignalErased` can carry them
/// across the type-erased boundary without losing const-correctness.
template<typename... Args>
class TypedSignal {
public:
    using Handler = std::function<void(const Args&...)>;

    /// Args bundle passed via void* through the abi layer.
    struct Bundle {
        std::tuple<const Args&...> refs;
    };

    /// Attach a handler. The returned Subscription disconnects the slot
    /// on destruction (or on explicit `release()`).
    [[nodiscard]] ::aria::Subscription connect(Handler handler) {
        // The slot owns a heap-allocated copy of the handler; the
        // canonical factory in <aria/abi/slot_factory.hpp> takes care
        // of the trampoline + destroyer, including the noexcept
        // contract at the ABI boundary.
        const auto id = sig_.connect(abi::make_slot_for<Bundle>(
            [fn = std::move(handler)](const Bundle& b) {
                std::apply(
                    [&fn](const Args&... a) { fn(a...); },
                    b.refs);
            }));

        // The "disconnect-on-destroy" lambda is captured into the unified
        // Subscription. We use a weak reference to the signal's control
        // block so that disconnecting after the signal is gone is a safe
        // no-op (the whole reason abi::SignalErased keeps a control block).
        auto weak = sig_.weak_handle();
        return ::aria::Subscription{[weak, id]() noexcept {
            abi::SignalErased::disconnect_via_weak(weak, id);
        }};
    }

    void emit(const Args&... args) const {
        Bundle b{std::tuple<const Args&...>{args...}};
        sig_.emit(&b);
    }

    [[nodiscard]] std::size_t slot_count() const noexcept {
        return sig_.slot_count();
    }

    void clear() noexcept { sig_.clear(); }

    /// Exposed so niche users (e.g. the Qt adapter) can tie their own
    /// lifetime rules to the same control block.
    [[nodiscard]] std::weak_ptr<abi::SignalErased::ControlBlock>
    weak_handle() const noexcept {
        return sig_.weak_handle();
    }

private:
    mutable abi::SignalErased sig_;
};

}  // namespace aria::detail
