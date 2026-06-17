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

    /// Disconnect by id. Safe to call after the signal is destroyed.
    void disconnect(SlotId id) noexcept;

    /// Emit to all slots. The args pointer is passed to each slot's invoker.
    /// Slots are snapshotted under lock; emission happens outside the lock.
    void emit(void* args) const;

    /// Number of currently connected slots.
    [[nodiscard]] std::size_t slot_count() const noexcept;

    /// Drop all slots.
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
    struct Impl;
    // RAII pImpl. The shared_ptr<ControlBlock> lives inside Impl
    // (signal.cpp). A moved-from SignalErased has `impl_ == nullptr`; every
    // public method null-checks and degrades to a safe no-op (see .cpp).
    std::unique_ptr<Impl> impl_;
};

}  // namespace aria::abi
