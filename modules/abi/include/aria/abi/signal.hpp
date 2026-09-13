#pragma once

#include "export.hpp"
#include "slot.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace aria::abi {

/// Type-erased multi-cast signal. Thread-safe.
///
/// Stored as a control block reachable via shared_ptr — Subscriptions hold
/// a weak_ptr to it, so unsubscribing after the signal is destroyed is a no-op
/// rather than a crash.
class ARIA_ABI_API SignalErased {
public:
    SignalErased();
    ~SignalErased();

    SignalErased(const SignalErased&) = delete;
    SignalErased& operator=(const SignalErased&) = delete;
    SignalErased(SignalErased&&) noexcept;
    SignalErased& operator=(SignalErased&&) noexcept;

    /// Add a slot. Returns its id (used for disconnection).
    SlotId connect(SlotErased slot);

    /// Disconnect by id. Later callbacks in an active emission are skipped.
    /// A callback already executing on another thread may finish.
    void disconnect(SlotId id) noexcept;

    /// Emit to all slots. The args pointer is passed to each slot's invoker.
    /// Slots are snapshotted under lock; emission happens outside the lock.
    void emit(void* args) const;

    /// Number of currently connected slots.
    [[nodiscard]] std::size_t slot_count() const noexcept;

    /// Drop all slots, releasing captures outside the signal lock.
    void clear() noexcept;

    // ── Internal: weak control-block handle (used for safe disconnect) ──
    struct ControlBlock;
    [[nodiscard]] std::weak_ptr<ControlBlock> weak_handle() const noexcept;

    /// Disconnect a slot via a weak handle to the signal's control
    /// block — used by Subscription's RAII deleter so unsubscribing
    /// after the signal itself has been destroyed is a safe no-op.
    static void disconnect_via_weak(
        const std::weak_ptr<ControlBlock>& weak,
        SlotId id) noexcept;

private:
    std::shared_ptr<ControlBlock> cb_;
};

}  // namespace aria::abi
