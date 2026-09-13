#pragma once

// RecyclerView mirror for any ListSourceOf<L, T>. Snapshot changes and native
// notifications are delivered together, in source-event order. When producers
// run off the Android main thread, supply a dispatcher backed by that looper;
// posting only the notification would expose the wrong mirror to RecyclerView.
// This header has no JNI dependency and can be tested on a desktop host.

#include "aria/callback_boundary.hpp"
#include "aria/list_source.hpp"
#include "aria/runtime/dispatcher.hpp"
#include "aria/subscription.hpp"

#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace aria::adapters::jni {

enum class RecyclerNotify {
    ItemInserted,
    ItemRemoved,
    ItemChanged,
    ItemMoved,
    DataSetChanged,
};

/// position is the changed row (the destination for a move); from_position
/// is meaningful only for ItemMoved. Positions are sequential replay indices.
struct RecyclerNotification {
    RecyclerNotify kind = RecyclerNotify::DataSetChanged;
    std::size_t position = 0;
    std::size_t from_position = 0;
};

template<typename T>
class JniListSource {
public:
    using NotifySink = std::function<void(const RecyclerNotification&)>;

    /// Construct on the source's graph thread. The initial snapshot is ready
    /// before any notification. With no dispatcher, source events must already
    /// arrive on the RecyclerView owner thread. With a dispatcher, mirror edits
    /// and sink calls are posted together when that thread differs.
    ///
    /// The source can be destroyed first: its owning events and the initial
    /// snapshot are sufficient for all subsequent mirror operations. A queued
    /// callback cannot reach a destroyed bridge. Sink exceptions are reported
    /// through the callback failure sink and do not escape the native boundary.
    template<class L>
        requires ::aria::ListSourceOf<L, T>
    JniListSource(L& source, NotifySink sink,
                  std::shared_ptr<runtime::IDispatcher> dispatcher = {})
        : state_(std::make_shared<State>(source.snapshot(), std::move(sink),
                                        std::move(dispatcher))) {
        sub_ = source.observe([weak = std::weak_ptr<State>(state_)](const ListChange<T>& change) {
            if (auto state = weak.lock()) enqueue_(state, change);
        });
    }

    ~JniListSource() {
        close_(state_);
        sub_.release();
    }
    JniListSource(const JniListSource&) = delete;
    JniListSource& operator=(const JniListSource&) = delete;

    [[nodiscard]] std::size_t item_count() const {
        std::lock_guard lock(state_->mutex);
        return state_->rows.size();
    }

    /// Out-of-range reads return nullptr. Rows are owning shared pointers;
    /// callers must still obey T's own rules when reading mutable item fields.
    [[nodiscard]] std::shared_ptr<T> at(std::size_t position) const {
        std::lock_guard lock(state_->mutex);
        return position < state_->rows.size() ? state_->rows[position] : nullptr;
    }

    /// Ask a reattached RecyclerView to redisplay the current event mirror.
    /// Queued changes are applied before this notification; no source reread
    /// can accidentally apply future changes before their own notifications.
    void reload() {
        auto state = state_;
        enqueue_(state, std::nullopt);
    }

private:
    using Rows = std::vector<std::shared_ptr<T>>;
    // nullopt is a reload request, ordered with normal changes in the queue.
    using Event = std::optional<ListChange<T>>;
    struct State {
        State(Rows initial, NotifySink callback, std::shared_ptr<runtime::IDispatcher> owner)
            : rows(std::move(initial)), sink(std::make_shared<NotifySink>(std::move(callback))),
              dispatcher(std::move(owner)) {}
        std::mutex mutex;
        bool active = true;
        bool scheduled = false;
        Rows rows;
        std::shared_ptr<NotifySink> sink;
        std::shared_ptr<runtime::IDispatcher> dispatcher;
        std::deque<Event> pending;
    };

    static void close_(const std::shared_ptr<State>& state) noexcept {
        Rows retired_rows;
        std::shared_ptr<NotifySink> retired_sink;
        std::deque<Event> retired_events;
        {
            std::lock_guard lock(state->mutex);
            state->active = false;
            retired_rows.swap(state->rows);
            retired_sink.swap(state->sink);
            retired_events.swap(state->pending);
        }
        // Items and callback captures may re-enter the bridge on destruction.
    }

    static void enqueue_(const std::shared_ptr<State>& state, Event event) {
        std::shared_ptr<runtime::IDispatcher> dispatcher;
        {
            std::lock_guard lock(state->mutex);
            if (!state->active) return;
            state->pending.push_back(std::move(event));
            if (state->scheduled) return;
            state->scheduled = true;
            dispatcher = state->dispatcher;
        }
        if (dispatcher && !dispatcher->is_main_thread()) {
            try {
                dispatcher->post([weak = std::weak_ptr<State>(state)] {
                    if (auto owner = weak.lock()) drain_(owner);
                });
            } catch (...) {
                std::deque<Event> retired;
                {
                    std::lock_guard lock(state->mutex);
                    state->scheduled = false;
                    retired.swap(state->pending);
                }
                report_callback_failure("jni.list_source.dispatch", std::current_exception());
            }
        } else {
            drain_(state);
        }
    }

    static void drain_(const std::shared_ptr<State>& state) noexcept {
        while (true) {
            Event event;
            {
                std::lock_guard lock(state->mutex);
                if (!state->active || state->pending.empty()) {
                    state->scheduled = false;
                    return;
                }
                event = std::move(state->pending.front());
                state->pending.pop_front();
            }
            try {
                apply_(state, event);
            } catch (...) {
                report_callback_failure("jni.list_source.change", std::current_exception());
            }
        }
    }

    static void apply_(const std::shared_ptr<State>& state, const Event& event) {
        using K = ListChangeKind;
        Rows reset_rows;
        if (event && event->kind == K::Reset) {
            if (!event->snapshot) throw std::invalid_argument("JniListSource: Reset needs a snapshot");
            reset_rows = *event->snapshot;
        }
        std::shared_ptr<T> retired_item;
        std::shared_ptr<NotifySink> sink;
        RecyclerNotification notification;
        {
            std::lock_guard lock(state->mutex);
            if (!state->active) return;
            auto& rows = state->rows;
            if (event) {
                const auto& change = *event;
                const auto index = change.index;
                switch (change.kind) {
                case K::Insert:
                    if (index > rows.size()) throw std::out_of_range("JniListSource: insert index");
                    rows.insert(rows.begin() + static_cast<std::ptrdiff_t>(index), change.item);
                    notification = {RecyclerNotify::ItemInserted, index, 0};
                    break;
                case K::Remove:
                    if (index >= rows.size()) return;
                    retired_item = std::move(rows[index]);
                    rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(index));
                    notification = {RecyclerNotify::ItemRemoved, index, 0};
                    break;
                case K::Replace:
                case K::ItemChanged:
                    if (index >= rows.size()) return;
                    retired_item = std::move(rows[index]);
                    rows[index] = change.item;
                    notification = {RecyclerNotify::ItemChanged, index, 0};
                    break;
                case K::Move: {
                    const auto from = change.from_index;
                    if (from >= rows.size() || index >= rows.size() || from == index) return;
                    auto moved = std::move(rows[from]);
                    rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(from));
                    rows.insert(rows.begin() + static_cast<std::ptrdiff_t>(index), std::move(moved));
                    notification = {RecyclerNotify::ItemMoved, index, from};
                    break;
                }
                case K::Reset:
                    rows.swap(reset_rows);
                    break;
                }
            }
            sink = state->sink;
        }
        // Keep the sink alive independently: it may destroy the bridge while
        // running. Retired row objects are also released outside the mutex.
        if (sink && *sink) (*sink)(notification);
    }

    std::shared_ptr<State> state_;
    Subscription sub_;
};

} // namespace aria::adapters::jni
