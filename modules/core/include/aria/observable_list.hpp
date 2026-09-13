#pragma once

#include "aria/subscription.hpp"
#include "aria/diagnostics.hpp"
#include "aria/detail/list_signal_mixin.hpp"
#include "aria/detail/list_signal.hpp"
#include "aria/list_change.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aria {

/// Observable sequence of owning element handles. Readers take a shared lock;
/// writers serialize mutation and event publication with a separate recursive
/// lock. Callbacks execute without the structural lock. Event payloads own the
/// affected objects and are replayed in order even when the source has already
/// committed the final state of a batch (see ListChange).
///
/// Reentrant writes commit immediately, then notify after the current complete
/// event/batch fan-out. Shared backing state keeps an in-flight operation safe
/// if an observer destroys the ObservableList itself.
///
/// Repeated handles are separate sequence occurrences. index_of returns the
/// last occurrence; one item subscription emits ItemChanged for each current
/// occurrence. Unique-item lookup and append are amortized O(1); middle edits
/// shift a contiguous vector. Range edits shift/compact once, in O(n + k).
template<typename T>
class ObservableList : public detail::ListSignalMixin<ObservableList<T>, T> {
    friend detail::ListSignalMixin<ObservableList<T>, T>;
    using Event = ListChange<T>;
    struct Record {
        std::size_t index = 0;
        std::size_t count = 0;
        bool installing = false;
        Subscription subscription;
    };
    struct SharedState {
        mutable std::shared_mutex mutex;
        std::recursive_mutex writer;
        std::vector<std::shared_ptr<T>> items;
        std::unordered_map<const T*, Record> records;
        std::shared_ptr<detail::ListSignal<T>> signal = std::make_shared<detail::ListSignal<T>>();
    };

public:
    using value_type = T;
    using Signal = detail::ListSignal<T>;
    ObservableList() = default;
    ObservableList(const ObservableList&) = delete;
    ObservableList& operator=(const ObservableList&) = delete;
    ObservableList(ObservableList&&) = delete;
    ObservableList& operator=(ObservableList&&) = delete;

    [[nodiscard]] std::size_t size() const { return size_(state_); }
    [[nodiscard]] bool empty() const { return size() == 0; }
    [[nodiscard]] std::shared_ptr<T> at(std::size_t index) const {
        std::shared_lock lock(state_->mutex);
        return state_->items.at(index);
    }
    [[nodiscard]] std::vector<std::shared_ptr<T>> snapshot() const {
        std::shared_lock lock(state_->mutex);
        return state_->items;
    }

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

    void push_back(std::shared_ptr<T> item) {
        auto state = state_;
        std::lock_guard sequence(state->writer);
        std::size_t index;
        {
            std::unique_lock lock(state->mutex);
            index = state->items.size();
            state->items.push_back(item);
            auto& record = state->records[item.get()];
            record.index = index;
            ++record.count;
        }
        emit_(state, Event{ListChangeKind::Insert, index, item, 0}, index + 1,
              [state, item] { install_(state, item); });
    }

    template<typename... Args>
    std::shared_ptr<T> emplace_back(Args&&... args) {
        auto item = std::make_shared<T>(std::forward<Args>(args)...);
        push_back(item);
        return item;
    }

    void insert(std::size_t index, std::shared_ptr<T> item) {
        auto state = state_;
        std::lock_guard sequence(state->writer);
        {
            std::unique_lock lock(state->mutex);
            index = std::min(index, state->items.size());
            state->items.insert(state->items.begin() + static_cast<std::ptrdiff_t>(index), item);
            ++state->records[item.get()].count;
            reindex_(state, index);
        }
        emit_(state, Event{ListChangeKind::Insert, index, item, 0}, size_(state),
              [state, item] { install_(state, item); });
    }

    /// One O(n + k) insertion and k owning Insert events, in forward order.
    template<typename InputIt>
    void insert_range(std::size_t index, InputIt first, InputIt last) {
        auto state = state_;
        std::lock_guard sequence(state->writer);
        std::vector<std::shared_ptr<T>> pending(first, last);
        if (pending.empty()) return;
        std::vector<Event> events;
        events.reserve(pending.size());
        {
            std::unique_lock lock(state->mutex);
            index = std::min(index, state->items.size());
            state->items.insert(state->items.begin() + static_cast<std::ptrdiff_t>(index),
                                pending.begin(), pending.end());
            for (std::size_t i = 0; i < pending.size(); ++i) {
                ++state->records[pending[i].get()].count;
                events.push_back({ListChangeKind::Insert, index + i, pending[i], 0});
            }
            reindex_(state, index);
        }
        emit_batch_(state, std::move(events), size_(state), [&] {
            for (const auto& item : pending) install_(state, item);
        });
    }

    void remove_at(std::size_t index) {
        auto state = state_;
        std::lock_guard sequence(state->writer);
        std::shared_ptr<T> removed;
        Subscription detached;
        {
            std::unique_lock lock(state->mutex);
            if (index >= state->items.size()) return;
            removed = state->items[index];
            state->items.erase(state->items.begin() + static_cast<std::ptrdiff_t>(index));
            detached = drop_(state, removed.get());
            reindex_(state, index);
        }
        emit_(state, Event{ListChangeKind::Remove, index, removed, 0}, size_(state));
    }

    /// Removes in forward event order, each at the same replay pivot.
    void remove_range(std::size_t index, std::size_t count) {
        auto state = state_;
        std::lock_guard sequence(state->writer);
        std::vector<Event> events;
        std::vector<Subscription> detached;
        {
            std::unique_lock lock(state->mutex);
            if (index >= state->items.size()) return;
            count = std::min(count, state->items.size() - index);
            if (count == 0) return;
            events.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                events.push_back({ListChangeKind::Remove, index, state->items[index + i], 0});
            }
            bool repair_prefix = false;
            for (const auto& event : events) {
                const auto record = state->records.find(event.item.get());
                if (--record->second.count == 0) {
                    if (record->second.subscription) detached.push_back(std::move(record->second.subscription));
                    state->records.erase(record);
                } else if (record->second.index >= index && record->second.index < index + count) {
                    record->second.index = std::numeric_limits<std::size_t>::max();
                    repair_prefix = true;
                }
            }
            const auto begin = state->items.begin() + static_cast<std::ptrdiff_t>(index);
            state->items.erase(begin, begin + static_cast<std::ptrdiff_t>(count));
            reindex_(state, index);
            if (repair_prefix) {
                for (std::size_t i = index; i-- > 0;) {
                    auto& record = state->records.at(state->items[i].get());
                    if (record.index == std::numeric_limits<std::size_t>::max()) record.index = i;
                }
            }
        }
        emit_batch_(state, std::move(events), size_(state));
    }

    template<std::predicate<const T&> Pred>
    bool remove_first(Pred&& predicate) {
        auto state = state_;
        std::lock_guard sequence(state->writer);
        std::size_t index;
        {
            std::shared_lock lock(state->mutex);
            index = 0;
            while (index < state->items.size() && !predicate(*state->items[index])) ++index;
            if (index == state->items.size()) return false;
        }
        ObservableList current{state};
        current.remove_at(index);
        return true;
    }

    template<std::predicate<const T&> Pred>
    std::size_t remove_all(Pred&& predicate) {
        auto state = state_;
        std::lock_guard sequence(state->writer);
        std::vector<Event> events;
        std::vector<Subscription> detached;
        {
            std::unique_lock lock(state->mutex);
            std::vector<bool> remove;
            remove.reserve(state->items.size());
            // User code completes before any item/map is moved. A throwing
            // predicate leaves the sequence and all subscriptions untouched.
            std::size_t removed_count = 0;
            std::size_t subscriptions = 0;
            for (const auto& item : state->items) {
                const bool selected = predicate(*item);
                remove.push_back(selected);
                if (selected) {
                    ++removed_count;
                    if constexpr (requires(T& value) { value.on_changed(std::declval<std::function<void(const T&)>>()); }) {
                        if (state->records.at(item.get()).subscription) ++subscriptions;
                    }
                }
            }
            // Allocate event/detachment storage before changing any rows.
            events.reserve(removed_count);
            detached.reserve(subscriptions);
            std::size_t write = 0;
            for (std::size_t read = 0; read < state->items.size(); ++read) {
                if (remove[read]) {
                    events.push_back({ListChangeKind::Remove, write, state->items[read], 0});
                    const auto record = state->records.find(state->items[read].get());
                    if (--record->second.count == 0) {
                        if (record->second.subscription) detached.push_back(std::move(record->second.subscription));
                        state->records.erase(record);
                    }
                } else {
                    state->records.at(state->items[read].get()).index = write;
                    if (read != write) state->items[write] = std::move(state->items[read]);
                    ++write;
                }
            }
            state->items.erase(state->items.begin() + static_cast<std::ptrdiff_t>(write), state->items.end());
        }
        const auto count = events.size();
        emit_batch_(state, std::move(events), size_(state));
        return count;
    }

    void replace_at(std::size_t index, std::shared_ptr<T> item) {
        auto state = state_;
        std::lock_guard sequence(state->writer);
        std::shared_ptr<T> previous;
        Subscription detached;
        {
            std::unique_lock lock(state->mutex);
            if (index >= state->items.size()) return;
            previous = std::exchange(state->items[index], item);
            if (previous != item) {
                detached = drop_(state, previous.get());
                auto& record = state->records[item.get()];
                record.index = record.count ? std::max(record.index, index) : index;
                ++record.count;
            }
        }
        emit_(state, Event{ListChangeKind::Replace, index, item, 0}, size_(state),
              [state, item] { install_(state, item); });
    }

    void move(std::size_t from, std::size_t to) {
        auto state = state_;
        std::lock_guard sequence(state->writer);
        std::shared_ptr<T> item;
        {
            std::unique_lock lock(state->mutex);
            auto& items = state->items;
            if (from == to || from >= items.size() || to >= items.size()) return;
            item = items[from];
            const auto first = items.begin() + static_cast<std::ptrdiff_t>(std::min(from, to));
            const auto last = items.begin() + static_cast<std::ptrdiff_t>(std::max(from, to)) + 1;
            std::rotate(first, from < to ? first + 1 : last - 1, last);
            reindex_(state, std::min(from, to));
        }
        emit_(state, Event{ListChangeKind::Move, to, item, from}, size_(state));
    }

    void clear() {
        auto state = state_;
        std::lock_guard sequence(state->writer);
        std::vector<std::shared_ptr<T>> removed;
        std::unordered_map<const T*, Record> detached;
        {
            std::unique_lock lock(state->mutex);
            removed.swap(state->items);
            detached.swap(state->records);
        }
        emit_(state, Event::cleared(), 0);
    }

    struct AddressIdentity {
        const void* operator()(const T& value) const noexcept { return &value; }
    };

    /// Reconcile by unique key. Null targets are ignored. Duplicate target
    /// keys use a Reset/rebuild. No-op and append are expected O(n); arbitrary
    /// vector reordering is O(n²). The entire edit stream is published as one
    /// batch, so a subscriber cannot invalidate the remaining diff midway.
    template<typename KeyFn = AddressIdentity>
    std::size_t reconcile(std::vector<std::shared_ptr<T>> next, KeyFn key_of = {}) {
        using Key = std::decay_t<std::invoke_result_t<KeyFn, const T&>>;
        auto state = state_;
        std::lock_guard sequence(state->writer);
        ObservableList current{state};
        std::erase(next, std::shared_ptr<T>{});
        std::unordered_map<Key, std::size_t> wanted;
        wanted.reserve(next.size());
        bool duplicates = false;
        for (std::size_t i = 0; i < next.size(); ++i) {
            if (!wanted.emplace(key_of(*next[i]), i).second) duplicates = true;
        }
        std::size_t count = 0;
        state->signal->batch([&] {
            if (duplicates) {
                current.clear();
                current.insert_range(0, next.begin(), next.end());
                count = next.size() + 1;
            } else {
                const auto before = current.snapshot();
                for (std::size_t i = before.size(); i-- > 0;) {
                    if (!wanted.contains(key_of(*before[i]))) { current.remove_at(i); ++count; }
                }
                for (std::size_t target = 0; target < next.size(); ++target) {
                    const auto key = key_of(*next[target]);
                    std::size_t found;
                    std::size_t length;
                    std::shared_ptr<T> item;
                    {
                        std::shared_lock lock(state->mutex);
                        length = state->items.size();
                        found = target;
                        while (found < length && key_of(*state->items[found]) != key) ++found;
                        if (found < length) item = state->items[found];
                    }
                    if (found == length) { current.insert(target, next[target]); ++count; }
                    else {
                        if (found != target) { current.move(found, target); ++count; }
                        if (item != next[target]) { current.replace_at(target, next[target]); ++count; }
                    }
                }
                while (current.size() > next.size()) { current.remove_at(current.size() - 1); ++count; }
            }
        });
        return count;
    }

    [[nodiscard]] std::size_t index_of(const T* item) const {
        std::shared_lock lock(state_->mutex);
        const auto found = state_->records.find(item);
        return found == state_->records.end() ? state_->items.size() : found->second.index;
    }
    [[nodiscard]] bool contains(const T* item) const {
        std::shared_lock lock(state_->mutex);
        return state_->records.contains(item);
    }

private:
    std::shared_ptr<SharedState> state_ = std::make_shared<SharedState>();
    std::shared_ptr<Signal> signal_ = state_->signal;
    explicit ObservableList(std::shared_ptr<SharedState> state)
        : state_(std::move(state)), signal_(state_->signal) {}

    static std::size_t size_(const std::shared_ptr<SharedState>& state) {
        std::shared_lock lock(state->mutex);
        return state->items.size();
    }
    static void reindex_(const std::shared_ptr<SharedState>& state, std::size_t from) {
        for (std::size_t i = from; i < state->items.size(); ++i) state->records.at(state->items[i].get()).index = i;
    }
    // Caller holds the structural lock; release returned subscriptions outside.
    static Subscription drop_(const std::shared_ptr<SharedState>& state, const T* item) {
        const auto found = state->records.find(item);
        if (--found->second.count == 0) {
            auto subscription = std::move(found->second.subscription);
            state->records.erase(found);
            return subscription;
        }
        // Only duplicate removals require this fallback. Unique tail removal
        // stays O(1), while repeated handles retain their last valid index.
        for (std::size_t i = state->items.size(); i-- > 0;) {
            if (state->items[i].get() == item) { found->second.index = i; break; }
        }
        return {};
    }
    template<typename U = T>
    static auto subscribe_(const std::shared_ptr<SharedState>& owner, U* item)
        -> decltype(item->on_changed(std::declval<std::function<void(const U&)>>()), Subscription{}) {
        std::weak_ptr<SharedState> weak = owner;
        const T* raw = item;
        return item->on_changed([weak, raw](const U&) {
            const auto state = weak.lock();
            if (!state) return;
            std::lock_guard sequence(state->writer);
            Event event;
            std::vector<Event> repeated;
            {
                std::shared_lock lock(state->mutex);
                const auto found = state->records.find(raw);
                if (found == state->records.end()) return;
                if (found->second.count == 1) {
                    const auto index = found->second.index;
                    event = Event{ListChangeKind::ItemChanged, index, state->items[index], 0};
                } else {
                    repeated.reserve(found->second.count);
                    for (std::size_t i = 0; i < state->items.size(); ++i) {
                        if (state->items[i].get() == raw) repeated.push_back({ListChangeKind::ItemChanged, i, state->items[i], 0});
                    }
                }
            }
            if (repeated.empty()) emit_(state, std::move(event), size_(state));
            else emit_batch_(state, std::move(repeated), size_(state));
        });
    }
    static Subscription subscribe_(const std::shared_ptr<SharedState>&, ...) { return {}; }

    static void install_(const std::shared_ptr<SharedState>& state, const std::shared_ptr<T>& item) {
        if (!item) return;
        {
            std::unique_lock lock(state->mutex);
            const auto found = state->records.find(item.get());
            if (found == state->records.end() || found->second.subscription || found->second.installing) return;
            found->second.installing = true;
        }
        Subscription subscription;
        try { subscription = subscribe_(state, item.get()); }
        catch (...) {
            std::unique_lock lock(state->mutex);
            const auto found = state->records.find(item.get());
            if (found != state->records.end()) found->second.installing = false;
            throw;
        }
        std::unique_lock lock(state->mutex);
        const auto found = state->records.find(item.get());
        if (found == state->records.end()) return;
        found->second.installing = false;
        found->second.subscription = std::move(subscription);
    }

    static void trace_(const Event& event, std::size_t length) {
        if (!::aria::has_trace_sink()) return;
        const char* name = "Reset";
        switch (event.kind) {
        case ListChangeKind::Insert: name = "Insert"; break;
        case ListChangeKind::Remove: name = "Remove"; break;
        case ListChangeKind::Replace: name = "Replace"; break;
        case ListChangeKind::Move: name = "Move"; break;
        case ListChangeKind::ItemChanged: name = "ItemChanged"; break;
        case ListChangeKind::Reset: break;
        }
        ::aria::trace::List payload{std::string{name}, event.index, event.from_index, length};
        ::aria::publish_trace_unchecked(::aria::TraceCategory::List, std::move(payload));
    }
    template<typename Prepare = decltype([] {})>
    static void emit_(const std::shared_ptr<SharedState>& state, Event event,
                      std::size_t length, Prepare prepare = {}) {
        state->signal->emit(event, std::move(prepare));
        trace_(event, length);
    }
    template<typename Prepare = decltype([] {})>
    static void emit_batch_(const std::shared_ptr<SharedState>& state, std::vector<Event> events,
                            std::size_t length, Prepare prepare = {}) {
        if (::aria::has_trace_sink()) {
            state->signal->emit_batch(events, std::move(prepare));
            for (const auto& event : events) trace_(event, length);
        } else state->signal->emit_batch(std::move(events), std::move(prepare));
    }

};

} // namespace aria
