#include "aria/abi/signal.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <utility>

namespace aria::abi {

struct Entry {
    SlotId id;
    SlotErased slot;
    std::atomic<bool> active{true};

    Entry(SlotId key, SlotErased callback) noexcept
        : id(key), slot(std::move(callback)) {}
};

struct SignalErased::ControlBlock {
    mutable std::mutex mutex;
    std::vector<std::shared_ptr<Entry>> entries;
    std::uint64_t next_id = 1;
    bool closed = false;

    ControlBlock() { entries.reserve(2); }
};

namespace {
void clear_slots(const std::shared_ptr<SignalErased::ControlBlock>& state,
                 bool close = false) noexcept {
    if (!state) return;
    std::vector<std::shared_ptr<Entry>> removed;
    {
        std::lock_guard lock(state->mutex);
        if (close) state->closed = true;
        for (const auto& entry : state->entries)
            entry->active.store(false, std::memory_order_release);
        removed.swap(state->entries);
    }
}

void disconnect_slot(const std::shared_ptr<SignalErased::ControlBlock>& state,
                     SlotId id) noexcept {
    if (!state || !id.valid()) return;
    std::shared_ptr<Entry> removed;
    {
        std::lock_guard lock(state->mutex);
        const auto it = std::find_if(state->entries.begin(), state->entries.end(),
            [id](const auto& entry) { return entry->id == id; });
        if (it == state->entries.end()) return;
        (*it)->active.store(false, std::memory_order_release);
        removed = std::move(*it);
        state->entries.erase(it);
    }
}
}  // namespace

SignalErased::SignalErased() : cb_(std::make_shared<ControlBlock>()) {}
SignalErased::~SignalErased() { clear_slots(cb_, true); }
SignalErased::SignalErased(SignalErased&& other) noexcept = default;
SignalErased& SignalErased::operator=(SignalErased&& other) noexcept {
    if (this != &other) {
        auto removed = std::exchange(cb_, std::move(other.cb_));
        clear_slots(removed, true);
    }
    return *this;
}

SlotId SignalErased::connect(SlotErased slot) {
    const auto state = cb_;
    if (!state || slot.empty()) return {};
    auto entry = std::make_shared<Entry>(SlotId{}, std::move(slot));
    std::lock_guard lock(state->mutex);
    if (state->closed) return {};
    if (state->next_id == 0)
        throw std::overflow_error("SignalErased slot identifiers exhausted");
    entry->id = SlotId{state->next_id++};
    // Retain the local owner until unlocking, including allocation failures.
    state->entries.push_back(entry);
    return entry->id;
}

void SignalErased::disconnect(SlotId id) noexcept {
    const auto state = cb_;
    disconnect_slot(state, id);
}

void SignalErased::emit(void* args) const {
    const auto state = cb_;
    if (!state) return;
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
    using Snapshot = std::vector<std::shared_ptr<Entry>>;
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
        std::lock_guard lock(state->mutex);
        snap.reserve(state->entries.size());
        for (const auto& entry : state->entries) snap.push_back(entry);
    }
    for (const auto& entry : snap) {
        if (entry->active.load(std::memory_order_acquire)) entry->slot.invoke(args);
    }
}

std::size_t SignalErased::slot_count() const noexcept {
    if (!cb_) return 0;
    std::lock_guard lock(cb_->mutex);
    return cb_->entries.size();
}

void SignalErased::clear() noexcept {
    const auto state = cb_;
    clear_slots(state);
}

std::weak_ptr<SignalErased::ControlBlock> SignalErased::weak_handle() const noexcept {
    return cb_;
}

void SignalErased::disconnect_via_weak(
        const std::weak_ptr<ControlBlock>& weak, SlotId id) noexcept {
    disconnect_slot(weak.lock(), id);
}

}  // namespace aria::abi
