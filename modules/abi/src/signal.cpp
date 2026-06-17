#include "aria/abi/signal.hpp"

#include <algorithm>
#include <atomic>
#include <memory>

namespace aria::abi {

struct Entry {
    SlotId id;
    // Held by shared_ptr so emit() can snapshot a small vector of pointers
    // and invoke them OUTSIDE the lock.  Crucial: a slot may release a
    // Subscription as part of its callback (e.g. an AutoComputed dropping
    // its old dep subs during recompute), which would deadlock if we held
    // the signal's mutex while invoking.
    std::shared_ptr<SlotErased> slot;
};

struct SignalErased::ControlBlock {
    mutable std::mutex mutex;
    std::vector<Entry> entries;
    std::atomic<std::uint64_t> next_id{1};

    // Reserve room for the typical observer count. Most UI signals carry
    // 1–2 slots (one binding, occasionally a diagnostic tap); pre-reserving
    // 2 avoids the first reallocation for that case without over-allocating
    // for the long tail of single-observer signals.
    ControlBlock() { entries.reserve(2); }
};

struct SignalErased::Impl {
    std::shared_ptr<ControlBlock> cb = std::make_shared<ControlBlock>();
};

SignalErased::SignalErased() : impl_(std::make_unique<Impl>()) {}
SignalErased::~SignalErased() = default;

// Move transfers ownership of the heap Impl, leaving the source in a
// destructible-only state (`impl_ == nullptr`). std::unique_ptr gives us
// the correct move semantics for free.
//
// IMPORTANT: a moved-from SignalErased has `impl_ == nullptr`. By the
// classic C++ contract a moved-from object is destructible-only, but
// in practice users sometimes hand the moved-from instance to other
// code (e.g. it sits in a container that gets queried later). Every
// public method below therefore null-checks `impl_` and degrades to
// a safe no-op rather than dereferencing a dangling control block.
SignalErased::SignalErased(SignalErased&& other) noexcept = default;
SignalErased& SignalErased::operator=(SignalErased&& other) noexcept = default;

SlotId SignalErased::connect(SlotErased slot) {
    if (!impl_) return SlotId{};   // moved-from -> no-op
    SlotId id{impl_->cb->next_id.fetch_add(1, std::memory_order_relaxed)};
    auto sp = std::make_shared<SlotErased>(std::move(slot));
    std::lock_guard lk(impl_->cb->mutex);
    impl_->cb->entries.push_back(Entry{id, std::move(sp)});
    return id;
}

void SignalErased::disconnect(SlotId id) noexcept {
    if (!impl_) return;
    if (!id.valid()) return;
    std::lock_guard lk(impl_->cb->mutex);
    auto& v = impl_->cb->entries;
    v.erase(std::remove_if(v.begin(), v.end(),
                [id](const Entry& e) noexcept { return e.id == id; }),
            v.end());
}

void SignalErased::emit(void* args) const {
    if (!impl_) return;
    // Snapshot under lock, invoke without lock — this lets slots safely
    // disconnect themselves (or release Subscriptions on this same signal)
    // without deadlocking on the recursive lock.
    //
    // Allocation: the snapshot buffer is drawn from a thread-local pool so
    // the common (non-reentrant) emit reuses the same heap block across
    // calls instead of allocating a fresh vector every time — this is the
    // hot path for high-frequency signals (ObservableList item changes,
    // Command can_execute, EventBus). emit() can re-enter itself (a slot's
    // callback may emit on this very signal), so we must NOT share one
    // static buffer: we keep a small stack of buffers keyed by re-entrancy
    // depth, each of which keeps its capacity between uses.
    using Snapshot = std::vector<std::shared_ptr<SlotErased>>;
    static thread_local std::vector<Snapshot> tl_pool;
    static thread_local std::size_t           tl_depth = 0;

    if (tl_depth >= tl_pool.size()) {
        tl_pool.emplace_back();
    }
    // Borrow the buffer for this depth by SWAPPING it into a local. We must
    // not hold a reference into `tl_pool` across the invoke loop: a
    // re-entrant emit at a deeper depth may `emplace_back` and reallocate
    // `tl_pool`, invalidating any outstanding element reference. Swapping
    // keeps the heap block (and its capacity) in `snap` for the duration of
    // this call, then returns it to the pool slot in the guard.
    Snapshot snap;
    snap.swap(tl_pool[tl_depth]);
    snap.clear();
    const std::size_t my_depth = tl_depth;
    ++tl_depth;
    // Restore depth and return the (capacity-preserving) buffer even if a
    // slot throws.
    struct DepthGuard {
        std::size_t& depth;
        Snapshot&    buf;
        std::size_t  slot_index;
        ~DepthGuard() {
            --depth;
            buf.clear();
            // tl_pool is still alive (thread_local); return the buffer so
            // its capacity is reused on the next emit at this depth.
            tl_pool[slot_index].swap(buf);
        }
    } depth_guard{tl_depth, snap, my_depth};

    {
        std::lock_guard lk(impl_->cb->mutex);
        snap.reserve(impl_->cb->entries.size());
        for (auto& e : impl_->cb->entries) snap.push_back(e.slot);
    }
    for (auto& s : snap) {
        if (s) s->invoke(args);
    }
}

std::size_t SignalErased::slot_count() const noexcept {
    if (!impl_) return 0;
    std::lock_guard lk(impl_->cb->mutex);
    return impl_->cb->entries.size();
}

void SignalErased::clear() noexcept {
    if (!impl_) return;
    std::lock_guard lk(impl_->cb->mutex);
    impl_->cb->entries.clear();
}

std::weak_ptr<SignalErased::ControlBlock> SignalErased::weak_handle() const noexcept {
    if (!impl_) return {};
    return impl_->cb;
}

void SignalErased::disconnect_via_weak(
        const std::weak_ptr<SignalErased::ControlBlock>& weak,
        SlotId id) noexcept {
    if (auto cb = weak.lock()) {
        std::lock_guard lk(cb->mutex);
        auto& v = cb->entries;
        v.erase(std::remove_if(v.begin(), v.end(),
                    [id](const Entry& e) noexcept { return e.id == id; }),
                v.end());
    }
}

}  // namespace aria::abi
