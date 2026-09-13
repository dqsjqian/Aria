#pragma once

#include "aria/detail/typed_signal.hpp"
#include "aria/list_change.hpp"

#include <cassert>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace aria::detail {

/// Non-recursive, batch-preserving list delivery. An idle single-event emit
/// uses no queue allocation; reentrant events reuse vector capacity. Neither
/// queue nor signal locks span user code. Shared state survives owner deletion.
/// Internal sequence numbers exclude already-committed events from new
/// subscriptions, including the undelivered remainder of an active batch.
template<typename T>
class ListSignal {
    using Event = ListChange<T>;
    using Sequence = std::uint64_t;
    struct Queued {
        Event event;
        Sequence sequence;
    };
    struct State {
        TypedSignal<Event, Sequence> signal;
        std::mutex mutex;
        std::vector<Queued> pending;
        std::size_t head = 0;
        Sequence tail = 0;
        bool emitting = false;
        std::size_t batching = 0;
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();

    static void drain_(const std::shared_ptr<State>& state) {
        for (;;) {
            Queued next;
            {
                std::lock_guard lock(state->mutex);
                if (state->head == state->pending.size()) {
                    state->pending.clear();
                    state->head = 0;
                    state->emitting = false;
                    return;
                }
                next = std::move(state->pending[state->head++]);
            }
            state->signal.emit(next.event, next.sequence);
        }
    }
    static void abandon_(const std::shared_ptr<State>& state) noexcept {
        std::lock_guard lock(state->mutex);
        state->pending.clear();
        state->head = 0;
        state->emitting = false;
    }

public:
    using Handler = std::function<void(const Event&)>;
    [[nodiscard]] Subscription connect(Handler fn) {
        auto state = state_;
        std::lock_guard lock(state->mutex);
        const auto watermark = state->tail;
        return state->signal.connect([watermark, fn = std::move(fn)](const Event& event, const Sequence& sequence) {
            if (sequence > watermark) fn(event);
        });
    }
    [[nodiscard]] std::size_t slot_count() const noexcept { return state_->signal.slot_count(); }
    void clear() noexcept { state_->signal.clear(); }

    /// Collect a producer operation without delaying sequence assignment.
    /// Subscriptions created by its preparation callbacks start after every
    /// change already committed, even before the collected batch is delivered.
    template<typename Fn>
    void batch(Fn fn) const {
        auto state = state_;
        {
            std::lock_guard lock(state->mutex);
            ++state->batching;
        }
        std::exception_ptr error;
        try { fn(); } catch (...) { error = std::current_exception(); }
        bool drain = false;
        {
            std::lock_guard lock(state->mutex);
            --state->batching;
            if (state->batching == 0 && !state->emitting && !state->pending.empty()) {
                state->emitting = true;
                drain = true;
            }
        }
        if (drain) {
            try { drain_(state); } catch (...) { abandon_(state); throw; }
        }
        if (error) std::rethrow_exception(error);
    }

    void emit(Event event) const { emit(std::move(event), [] {}); }

    template<typename Prepare>
    void emit(Event event, Prepare prepare) const {
        assert(event.kind != ListChangeKind::Reset || event.snapshot);
        auto state = state_;
        Sequence sequence;
        bool started;
        {
            std::lock_guard lock(state->mutex);
            sequence = ++state->tail;
            started = !state->emitting && state->batching == 0;
            if (started) state->emitting = true;
            else state->pending.push_back(Queued{std::move(event), sequence});
        }
        // Reserve the structural event before installing subscriptions that
        // may synchronously emit ItemChanged or reenter the source.
        if (!started) { prepare(); return; }
        std::exception_ptr preparation_error;
        try { prepare(); } catch (...) { preparation_error = std::current_exception(); }
        try {
            state->signal.emit(event, sequence);
            drain_(state);
        } catch (...) {
            abandon_(state);
            throw;
        }
        // A failing item subscription cannot conceal an already-committed
        // structural mutation from existing list observers.
        if (preparation_error) std::rethrow_exception(preparation_error);
    }

    void emit_batch(std::vector<Event> events) const { emit_batch(std::move(events), [] {}); }

    template<typename Prepare>
    void emit_batch(std::vector<Event> events, Prepare prepare) const {
        if (events.empty()) { prepare(); return; }
        auto state = state_;
        Sequence first;
        bool started;
        {
            std::lock_guard lock(state->mutex);
#ifndef NDEBUG
            for (const auto& event : events) assert(event.kind != ListChangeKind::Reset || event.snapshot);
#endif
            first = state->tail + 1;
            state->tail += static_cast<Sequence>(events.size());
            started = !state->emitting && state->batching == 0;
            if (started) state->emitting = true;
            else {
                state->pending.reserve(state->pending.size() + events.size());
                auto sequence = first;
                for (auto& event : events) state->pending.push_back(Queued{std::move(event), sequence++});
            }
        }
        if (!started) { prepare(); return; }
        std::exception_ptr preparation_error;
        try { prepare(); } catch (...) { preparation_error = std::current_exception(); }
        try {
            auto sequence = first;
            for (const auto& event : events) state->signal.emit(event, sequence++);
            drain_(state);
        } catch (...) {
            abandon_(state);
            throw;
        }
        if (preparation_error) std::rethrow_exception(preparation_error);
    }
};

} // namespace aria::detail
