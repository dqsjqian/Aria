#pragma once

#include "aria/subscription.hpp"
#include "aria/diagnostics.hpp"
#include "aria/detail/list_signal_mixin.hpp"
#include "aria/detail/typed_signal.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aria {

enum class ListChangeKind {
    Insert,       ///< single-item insert at `index`
    Remove,       ///< single-item remove at `index` (value of `index` is the slot as it existed just before removal)
    Replace,      ///< item at `index` was swapped for a new one
    ItemChanged,  ///< T's own on_changed fired (index = current position)
    Reset,        ///< full clear (index = 0, item = nullptr)
    Move          ///< item moved from `from_index` to `index`
};

template<typename T>
struct ListChange {
    ListChangeKind kind;
    std::size_t index = 0;
    const T* item = nullptr;
    /// Only populated when kind == Move. Records the source index the
    /// item was moved from; `index` is then the destination.
    std::size_t from_index = 0;
};

/// Observable collection of std::shared_ptr<T>.
/// Notifies on Insert/Remove/Replace/Move, on Reset (clear), and — if T
/// provides `Subscription on_changed(std::function<void(const T&)>)` —
/// on ItemChanged.
///
/// ── Threading contract (READ THIS) ───────────────────────────────────
/// `ObservableList` supports a **single-writer / multi-reader** model and
/// guarantees the following:
///
///  * **Readers are fully thread-safe, any thread, any time.** `size()`,
///    `empty()`, `at()`, `snapshot()` take a shared lock and may run
///    concurrently with each other and with a writer. A `snapshot()` is a
///    consistent point-in-time copy.
///  * **A single writer thread** may freely interleave with readers. All
///    structural state (`slots_`, `index_of_`) is mutated under a unique
///    lock, so readers never observe a torn list.
///  * **Multiple concurrent writer threads** are serialised for structural
///    integrity (the unique lock) AND for *event ordering*: each mutator
///    holds `emit_seq_` across its structural mutation and its
///    notification fan-out, so observers always see Insert/Remove/... in
///    the same order the mutations committed — there is no window where
///    two writers interleave their `emit_(...)` calls out of structural
///    order. (This costs one extra uncontended lock on the single-writer
///    fast path; it only actually serialises when >1 thread mutates.)
///
/// What is deliberately NOT promised: an index carried by a *past*
/// notification is only meaningful at the instant that notification fired.
/// `index_of_raw_` (used by the per-item `ItemChanged` path) returns the
/// item's CURRENT index or `size()` if it has since been removed; an
/// observer that caches an index across mutations must treat a later
/// `size()` sentinel as "stale, ignore". This is inherent to an
/// incremental change stream and is pinned by the list-diff contract.
///
/// Re-entrancy contract:
///  * Per-item subscriptions are installed OUTSIDE the list's structural
///    write lock, so a `T::on_changed(fn)` implementation that fires `fn`
///    synchronously at subscribe time (e.g. `Property::bind` does this
///    by design) will NOT deadlock against the surrounding mutation.
///  * The synchronous fire is delivered after the structural mutation
///    is visible to readers; the resulting `ItemChanged` notification
///    fires from the install path on the calling thread, between the
///    structural Insert/Replace event and any further mutation.
///  * Re-entrant calls into the list from within `T::on_changed` are
///    allowed but not recommended — typical usage observes only.
///
/// Complexity notes:
///  * `index_of_raw_` is O(1) amortised thanks to a parallel
///    `index_of_` hash map. This keeps per-item `on_changed` callbacks
///    cheap even for very large lists.
///  * `insert_range(pos, first, last)` / `remove_range(pos, count)` /
///    `remove_all(pred)` emit one event **per element** in observation
///    order (per `docs/list-diff-contract.md` LD-2 / LD-7); the
///    observable surface stays incremental rather than batched. `move`
///    is the exception -- a single `Move` event is emitted, never
///    `Remove + Insert`. See `docs/list-diff-contract.md` for the
///    authoritative event-sequence table.
///  * Mid-list `insert` / `remove_at` are O(N) (vector shift) by design:
///    the contiguous `slots_` keeps `at()` / `snapshot()` cache-friendly,
///    which is the dominant access pattern for UI lists. Bulk edits use
///    the range APIs; a future rope/segmented backing store is a possible
///    optimisation if a real workload needs large random mid-list churn.
///
/// Duplicate shared_ptr semantics:
///  * Inserting the same `std::shared_ptr<T>` twice (keyed on raw `T*`)
///    causes the later index to win in the O(1) lookup map. This is
///    intentional: the map models "logical slot → index", not "identity
///    → index". Users who need two distinct slots for the same logical
///    object must use two distinct `shared_ptr<T>` instances (e.g. via
///    `std::make_shared<T>(*other)` or similar).
///  * This behaviour is pinned down by a dedicated test. A future
///    slot-identity mechanism is a known follow-up discussion topic.
template<typename T>
class ObservableList
    : public detail::ListSignalMixin<ObservableList<T>, T> {
    friend detail::ListSignalMixin<ObservableList<T>, T>;

public:
    /// Element type. Lets adapter / concept code recover `T` without
    /// needing the full template signature; satisfies `aria::ListSource`.
    using value_type = T;
    using Signal = detail::TypedSignal<ListChange<T>>;

    ObservableList() : signal_(std::make_shared<Signal>()) {}

    // ObservableList holds a `std::shared_mutex` member, which is NOT
    // movable per the C++ standard. We therefore delete every special
    // member explicitly — earlier revisions wrote
    //     ObservableList(ObservableList&&) noexcept = default;
    // which is misleading: the compiler defines that constructor as
    // deleted (mutex member can't be moved) but the `= default;`
    // declaration suggests otherwise to readers.
    ObservableList(const ObservableList&) = delete;
    ObservableList& operator=(const ObservableList&) = delete;
    ObservableList(ObservableList&&) = delete;
    ObservableList& operator=(ObservableList&&) = delete;

    // ── Capacity ──────────────────────────────────────────────────────
    [[nodiscard]] std::size_t size() const {
        std::shared_lock lk(mutex_);
        return slots_.size();
    }
    [[nodiscard]] bool empty() const { return size() == 0; }

    // ── Element access ────────────────────────────────────────────────
    [[nodiscard]] std::shared_ptr<T> at(std::size_t i) const {
        std::shared_lock lk(mutex_);
        return slots_.at(i).item;
    }

    [[nodiscard]] std::vector<std::shared_ptr<T>> snapshot() const {
        std::shared_lock lk(mutex_);
        std::vector<std::shared_ptr<T>> out;
        out.reserve(slots_.size());
        for (auto& s : slots_) out.push_back(s.item);
        return out;
    }

    // ── Range access (std::ranges-compatible) ─────────────────────────
    //
    // `ObservableList` cannot safely expose raw iterators over its
    // internal `slots_`: those would be invalidated by any concurrent
    // mutation and would bypass the `mutex_`. Instead, `items()` returns a
    // self-contained `SnapshotRange` — a consistent point-in-time copy of
    // the element handles that owns its own storage and satisfies
    // `std::ranges::range` (and `std::ranges::view`-friendly
    // borrowed-iterator semantics). It composes with the standard range
    // algorithms and views:
    //
    //     for (auto& item : list.items()) { use(*item); }
    //     auto names = list.items()
    //                | std::views::transform([](auto& p){ return p->name; });
    //     auto n = std::ranges::count_if(list.items(),
    //                  [](auto& p){ return p->active; });
    //
    // The snapshot is taken once when `items()` is called; later mutations
    // are not reflected in an already-obtained range (by design — a range
    // pass over a live, lock-free-iterated concurrent container is not a
    // coherent operation).
    class SnapshotRange {
    public:
        using value_type = std::shared_ptr<T>;
        using const_iterator =
            typename std::vector<std::shared_ptr<T>>::const_iterator;
        using iterator = const_iterator;

        explicit SnapshotRange(std::vector<std::shared_ptr<T>> data) noexcept
            : data_(std::move(data)) {}

        [[nodiscard]] const_iterator begin() const noexcept { return data_.begin(); }
        [[nodiscard]] const_iterator end()   const noexcept { return data_.end(); }
        [[nodiscard]] std::size_t    size()  const noexcept { return data_.size(); }
        [[nodiscard]] bool           empty() const noexcept { return data_.empty(); }
        [[nodiscard]] const std::shared_ptr<T>& operator[](std::size_t i) const {
            return data_[i];
        }

    private:
        std::vector<std::shared_ptr<T>> data_;
    };

    /// Return a thread-safe, std::ranges-compatible snapshot range over the
    /// element handles. See `SnapshotRange` for semantics.
    [[nodiscard]] SnapshotRange items() const { return SnapshotRange{snapshot()}; }

    // ── Mutations ─────────────────────────────────────────────────────
    void push_back(std::shared_ptr<T> item) {
        std::lock_guard<std::recursive_mutex> seq(emit_seq_);
        std::size_t idx;
        const T* raw;
        {
            std::unique_lock lk(mutex_);
            idx = slots_.size();
            raw = item.get();
            // Install the slot WITHOUT a per-item subscription first;
            // subscribing can trigger an immediate callback (see
            // Property::bind) which would then take a shared lock on
            // our own mutex — a deadlock. install_item_subscription_()
            // below attaches the Subscription AFTER we release the
            // write lock.
            slots_.push_back(Slot{item, Subscription{}});
            index_of_[raw] = idx;
        }
        install_item_subscription_(raw);
        emit_(ListChange<T>{ListChangeKind::Insert, idx, raw, 0}, idx + 1);
    }

    template<typename... Args>
    std::shared_ptr<T> emplace_back(Args&&... args) {
        auto item = std::make_shared<T>(std::forward<Args>(args)...);
        push_back(item);
        return item;
    }

    void insert(std::size_t pos, std::shared_ptr<T> item) {
        std::lock_guard<std::recursive_mutex> seq(emit_seq_);
        const T* raw;
        {
            std::unique_lock lk(mutex_);
            if (pos > slots_.size()) pos = slots_.size();
            raw = item.get();
            slots_.insert(slots_.begin() + static_cast<std::ptrdiff_t>(pos),
                          Slot{item, Subscription{}});
            reindex_from_(pos);       // every slot at/after pos shifted by +1
        }
        install_item_subscription_(raw);
        emit_(ListChange<T>{ListChangeKind::Insert, pos, raw, 0}, size());
    }

    /// Insert a range of items starting at `pos`. Emits one `Insert`
    /// notification per item, with indices that reflect the list's
    /// state at the moment of THAT notification (so observers that
    /// update incrementally stay consistent).
    ///
    /// Accepts any input iterator whose value type is
    /// `std::shared_ptr<T>` (or convertible to it).
    template<typename InputIt>
    void insert_range(std::size_t pos, InputIt first, InputIt last) {
        std::lock_guard<std::recursive_mutex> seq(emit_seq_);
        std::vector<std::shared_ptr<T>> pending(first, last);
        if (pending.empty()) return;

        std::vector<std::pair<std::size_t, const T*>> to_emit;
        to_emit.reserve(pending.size());
        {
            std::unique_lock lk(mutex_);
            if (pos > slots_.size()) pos = slots_.size();
            for (std::size_t i = 0; i < pending.size(); ++i) {
                const T* raw = pending[i].get();
                slots_.insert(slots_.begin()
                                + static_cast<std::ptrdiff_t>(pos + i),
                              Slot{pending[i], Subscription{}});
                to_emit.emplace_back(pos + i, raw);
            }
            reindex_from_(pos);
        }
        // Subscriptions and emissions BOTH happen outside the write
        // lock — if T's on_changed triggers immediately (e.g. a
        // bind-style subscription), the callback may call back into
        // us via index_of_raw_, which takes a shared_lock.
        for (auto& [idx, raw] : to_emit) {
            install_item_subscription_(raw);
        }
        for (auto& [idx, raw] : to_emit) {
            emit_(ListChange<T>{ListChangeKind::Insert, idx, raw, 0}, size());
        }
    }

    void remove_at(std::size_t i) {
        std::lock_guard<std::recursive_mutex> seq(emit_seq_);
        std::shared_ptr<T> removed;
        {
            std::unique_lock lk(mutex_);
            if (i >= slots_.size()) return;
            removed = slots_[i].item;
            index_of_.erase(removed.get());
            slots_.erase(slots_.begin() + static_cast<std::ptrdiff_t>(i));
            reindex_from_(i);
        }
        emit_(ListChange<T>{ListChangeKind::Remove, i, removed.get(), 0}, size());
    }

    /// Remove `count` consecutive items starting at `pos`. Emits one
    /// `Remove` per element in forward order, each with the index as
    /// seen by the observer at that moment (i.e. always `pos`, because
    /// every prior remove shifts the tail left).
    void remove_range(std::size_t pos, std::size_t count) {
        std::lock_guard<std::recursive_mutex> seq(emit_seq_);
        if (count == 0) return;
        std::vector<std::shared_ptr<T>> removed;
        removed.reserve(count);
        {
            std::unique_lock lk(mutex_);
            if (pos >= slots_.size()) return;
            count = std::min(count, slots_.size() - pos);
            for (std::size_t i = 0; i < count; ++i) {
                removed.push_back(slots_[pos + i].item);
                index_of_.erase(removed.back().get());
            }
            slots_.erase(
                slots_.begin() + static_cast<std::ptrdiff_t>(pos),
                slots_.begin() + static_cast<std::ptrdiff_t>(pos + count));
            reindex_from_(pos);
        }
        for (auto& ptr : removed) {
            emit_(ListChange<T>{ListChangeKind::Remove, pos, ptr.get(), 0}, size());
        }
    }

    template<std::predicate<const T&> Pred>
    bool remove_first(Pred&& pred) {
        std::lock_guard<std::recursive_mutex> seq(emit_seq_);
        std::shared_ptr<T> removed;
        std::size_t idx = 0;
        {
            std::unique_lock lk(mutex_);
            for (std::size_t i = 0; i < slots_.size(); ++i) {
                if (pred(*slots_[i].item)) {
                    removed = slots_[i].item;
                    idx = i;
                    index_of_.erase(removed.get());
                    slots_.erase(slots_.begin() + static_cast<std::ptrdiff_t>(i));
                    reindex_from_(i);
                    break;
                }
            }
        }
        if (removed) {
            emit_(ListChange<T>{ListChangeKind::Remove, idx, removed.get(), 0}, size());
            return true;
        }
        return false;
    }

    template<std::predicate<const T&> Pred>
    std::size_t remove_all(Pred&& pred) {
        std::lock_guard<std::recursive_mutex> seq(emit_seq_);
        // We want to emit Remove notifications in the SAME ORDER as removals
        // happen, with each index expressed in the list's state just before
        // THAT notification fires.  Strategy: scan once under the write lock,
        // record "position in the progressively-shrinking list" for each hit,
        // then fire notifications outside the lock.
        std::vector<std::pair<std::size_t, std::shared_ptr<T>>> removed;
        {
            std::unique_lock lk(mutex_);
            std::size_t read = 0;
            std::size_t write = 0;
            while (read < slots_.size()) {
                if (pred(*slots_[read].item)) {
                    // Position `write` is where the item sits in the
                    // progressively-shrinking list at the moment of emit.
                    removed.emplace_back(write, slots_[read].item);
                    index_of_.erase(slots_[read].item.get());
                    ++read;
                } else {
                    if (write != read) slots_[write] = std::move(slots_[read]);
                    ++write;
                    ++read;
                }
            }
            slots_.erase(slots_.begin() + static_cast<std::ptrdiff_t>(write),
                         slots_.end());
            // Every surviving slot's index may have shifted — rebuild
            // the index map in one sweep (O(N) vs O(N*removed)).
            rebuild_index_map_();
        }
        // Emit in removal order (NOT reversed).  Each `first` is the index
        // as the observer would see the list right before this emit.
        for (auto& [idx, ptr] : removed) {
            emit_(ListChange<T>{ListChangeKind::Remove, idx, ptr.get(), 0}, size());
        }
        return removed.size();
    }

    /// Replace the item at `i`.  Emits a Replace notification.
    void replace_at(std::size_t i, std::shared_ptr<T> item) {
        std::lock_guard<std::recursive_mutex> seq(emit_seq_);
        const T* raw;
        std::shared_ptr<T> old;
        {
            std::unique_lock lk(mutex_);
            if (i >= slots_.size()) return;
            old = slots_[i].item;
            index_of_.erase(old.get());
            raw = item.get();
            // Release any prior item subscription BEFORE we take the
            // new one outside the lock. Assigning to slots_[i] would
            // destroy the old Subscription while we hold the write
            // lock, which can trigger signal disconnect callbacks; we
            // keep that work under the lock since it does not re-enter
            // into this list.
            slots_[i] = Slot{item, Subscription{}};
            index_of_[raw] = i;
        }
        install_item_subscription_(raw);
        emit_(ListChange<T>{ListChangeKind::Replace, i, raw, 0}, size());
    }

    /// Move the item currently at `from` to position `to`. Emits a
    /// single `Move` notification with both indices populated. Indices
    /// are expressed in the list state *after* the move (i.e. the
    /// observer's final picture).
    ///
    /// Out-of-range / no-op requests (from == to, or either index ≥ size)
    /// are silently ignored.
    void move(std::size_t from, std::size_t to) {
        std::lock_guard<std::recursive_mutex> seq(emit_seq_);
        const T* raw;
        {
            std::unique_lock lk(mutex_);
            if (from >= slots_.size() || to >= slots_.size() || from == to) return;
            Slot moved = std::move(slots_[from]);
            raw = moved.item.get();
            slots_.erase(slots_.begin() + static_cast<std::ptrdiff_t>(from));
            slots_.insert(slots_.begin() + static_cast<std::ptrdiff_t>(to),
                          std::move(moved));
            // A move shifts every slot between the two endpoints; easier
            // to just rebuild in one pass than to reason about the span.
            rebuild_index_map_();
        }
        emit_(ListChange<T>{ListChangeKind::Move, to, raw, from}, size());
    }

    void clear() {
        std::lock_guard<std::recursive_mutex> seq(emit_seq_);
        {
            std::unique_lock lk(mutex_);
            slots_.clear();
            index_of_.clear();
        }
        emit_(ListChange<T>{ListChangeKind::Reset, 0, nullptr, 0}, 0);
    }

private:
    struct Slot {
        std::shared_ptr<T> item;
        Subscription item_sub;
    };

    /// Single emission point: forwards the change to the typed signal
    /// (the local subscriber protocol) AND to the unified diagnostic
    /// sink (the cross-subsystem trace protocol). Per docs/diagnostics.md
    /// this is the ONE place where every list mutation surfaces; new
    /// mutations should call `emit_(...)` instead of touching `signal_`
    /// directly.
    ///
    /// `size_after` is the list size after the mutation completed. It
    /// is computed by the caller while still holding (or right after
    /// dropping) the write lock; we do not re-acquire the shared lock
    /// here because doing so on every mutation would be a 10x cost on
    /// hot paths.
    void emit_(const ListChange<T>& ch, std::size_t size_after) {
        signal_->emit(ch);
        if (!::aria::has_trace_sink()) return;
        const char* op = "Reset";
        switch (ch.kind) {
            case ListChangeKind::Insert:      op = "Insert"; break;
            case ListChangeKind::Remove:      op = "Remove"; break;
            case ListChangeKind::Replace:     op = "Replace"; break;
            case ListChangeKind::ItemChanged: op = "ItemChanged"; break;
            case ListChangeKind::Reset:       op = "Reset"; break;
            case ListChangeKind::Move:        op = "Move"; break;
        }
        ::aria::trace::List payload{
            std::string{op},
            ch.index,
            ch.from_index,
            size_after,
        };
        ::aria::publish_trace_unchecked(::aria::TraceCategory::List, std::move(payload));
    }

    /// SFINAE-based: subscribe to per-item changes if T exposes `on_changed`.
    ///
    /// The callback intentionally does NOT hold the list's mutex (not
    /// even a shared lock). It looks up the current index via the
    /// atomic-hot `index_of_` map behind a SEPARATE shared_mutex — but
    /// crucially, if user's `on_changed` calls back synchronously from
    /// within a mutation (e.g. Property::bind fires once on subscribe),
    /// the surrounding `push_back` / `insert_*` / `replace_at` caller
    /// will have released the write lock first (see those methods'
    /// implementations). The design rule is: "install_item_subscription_()
    /// is called OUTSIDE the write lock."
    template<typename U = T>
    auto subscribe_item_(U* item)
        -> decltype(item->on_changed(std::declval<std::function<void(const U&)>>()),
                    Subscription{}) {
        std::weak_ptr<Signal> weak_sig = signal_;
        const T* raw = item;
        auto* list_self = this;
        return item->on_changed([weak_sig, raw, list_self](const U&) {
            if (auto sig = weak_sig.lock()) {
                std::size_t idx = list_self->index_of_raw_(raw);
                sig->emit(ListChange<T>{ListChangeKind::ItemChanged, idx, raw, 0});
            }
        });
    }

    Subscription subscribe_item_(...) { return Subscription{}; }

    /// Build a per-item subscription outside the write lock, then
    /// attach it to the slot identified by `raw`. If the item was
    /// already removed or replaced by the time we come back, the
    /// fresh subscription is dropped on the floor (no slot to pin it
    /// to). Must be called AFTER the slot is visible in `slots_` and
    /// `index_of_` (i.e. after the write lock is released).
    void install_item_subscription_(const T* raw) {
        // Build subscription outside the lock. The user's `on_changed`
        // may fire synchronously here (e.g. Property::bind semantics);
        // that call ends up in our listener, which takes a
        // shared_lock in `index_of_raw_`. Because we are NOT holding
        // the write lock at this point, that shared_lock is granted
        // immediately.
        //
        // `raw_to_item_for_subscribe_` resolves the raw pointer to the
        // current shared_ptr under a shared lock.
        auto item = raw_to_item_for_subscribe_(raw);
        if (!item) return;    // item already gone
        Subscription sub = subscribe_item_(item.get());

        // Attach. Must re-check that `raw` is still the slot's item —
        // a concurrent mutation may have removed / replaced it.
        std::unique_lock lk(mutex_);
        auto it = index_of_.find(raw);
        if (it == index_of_.end()) return;    // item no longer in list
        auto& slot = slots_[it->second];
        if (slot.item.get() == raw) {
            slot.item_sub = std::move(sub);
        }
        // Otherwise the slot was replaced; the stale `sub` falls out
        // of scope and auto-disconnects.
    }

    /// Resolve a raw pointer back to its current shared_ptr under a
    /// shared lock. Returns null if the item is no longer in the list.
    std::shared_ptr<T> raw_to_item_for_subscribe_(const T* raw) const {
        std::shared_lock lk(mutex_);
        auto it = index_of_.find(raw);
        if (it == index_of_.end()) return nullptr;
        return slots_[it->second].item;
    }

    /// O(1) index lookup via the hash map. Returns list size
    /// ("past the end") if the item is no longer a member — observers
    /// should treat that as "stale, ignore".
    std::size_t index_of_raw_(const T* raw) const {
        std::shared_lock lk(mutex_);
        if (auto it = index_of_.find(raw); it != index_of_.end()) {
            return it->second;
        }
        return slots_.size();
    }

    /// After inserting / removing / replacing at `start`, every slot
    /// from `start` onwards may have a new linear index. Walk once and
    /// fix them. Called with the write lock held.
    void reindex_from_(std::size_t start) {
        for (std::size_t i = start; i < slots_.size(); ++i) {
            index_of_[slots_[i].item.get()] = i;
        }
    }

    /// Rebuild the index map from scratch. Cheaper than surgically
    /// updating it when many slots shifted (remove_all / move).
    /// Called with the write lock held.
    void rebuild_index_map_() {
        index_of_.clear();
        index_of_.reserve(slots_.size());
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            index_of_[slots_[i].item.get()] = i;
        }
    }

    mutable std::shared_mutex mutex_;
    /// Serialises the *mutation→emit* span across concurrent writer
    /// threads so observers see events in commit order (see the threading
    /// contract in the class doc). Uncontended — and therefore nearly
    /// free — on the single-writer fast path. It is a separate lock from
    /// `mutex_` (which guards structural state and is also taken by
    /// readers); holding both never deadlocks because the acquisition
    /// order is always emit_seq_ → mutex_, never the reverse, and readers
    /// never touch emit_seq_.
    ///
    /// `recursive_mutex` (not plain `mutex`) because the re-entrancy
    /// contract permits a user's synchronous `T::on_changed` callback —
    /// fired from `install_item_subscription_` while this lock is held —
    /// to call back into a list mutator on the SAME thread; a plain mutex
    /// would self-deadlock there.
    mutable std::recursive_mutex emit_seq_;
    std::vector<Slot> slots_;
    /// Parallel `T* -> current index` lookup table, kept in sync with
    /// `slots_` under `mutex_`. Makes per-item `ItemChanged` O(1) even
    /// for large lists.
    std::unordered_map<const T*, std::size_t> index_of_;
    std::shared_ptr<Signal> signal_;
};

}  // namespace aria
