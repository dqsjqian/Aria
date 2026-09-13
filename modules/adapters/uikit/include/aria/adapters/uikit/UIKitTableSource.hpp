#pragma once

// UIKitTableSource.hpp — bridge any aria list source onto UITableView.
//
// UIKit counterpart of `qt_list_model_adapter.hpp` and
// `AppKitTableSource.hpp`. Accepts any source satisfying
// `aria::ListSourceOf<L, T>` (ObservableList / FilteredList /
// SortedList / MappedList) and turns `ListChange<T>` events into:
//
//     Insert       -> [UITableView insertRowsAtIndexPaths:...:]
//     Remove       -> [UITableView deleteRowsAtIndexPaths:...:]
//     Replace      -> [UITableView reloadRowsAtIndexPaths:...:]
//     ItemChanged  -> [UITableView reloadRowsAtIndexPaths:...:]
//     Move         -> [UITableView moveRowAtIndexPath:toIndexPath:]
//     Reset        -> [UITableView reloadData]
//
// Header is .mm-only (UIKit imports). Header-only template — same
// distribution model as the AppKit / Qt6 adapters.
//
// Construct and read on the main thread. Events own their payloads and
// enter one FIFO queue, so a worker event cannot be overtaken by a later
// main-thread event. Idle main-thread delivery remains synchronous.
// Destruction retires pending work immediately; native data-source cleanup
// is transferred to the main queue when destroyed from another thread.

#include "aria/callback_boundary.hpp"
#include "aria/list_source.hpp"
#include "aria/observable_list.hpp"
#include "aria/subscription.hpp"

#import <UIKit/UIKit.h>

#include <atomic>
#include <cstddef>
#include <functional>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <memory>
#include <utility>
#include <vector>

// ─── ObjC data-source / delegate ────────────────────────────────────────

@interface AriaUITableDataSource : NSObject <UITableViewDataSource, UITableViewDelegate>
- (instancetype)initWithRowCount:(std::function<NSInteger()>)rowCountFn
                       cellForFn:(std::function<UITableViewCell*(UITableView*,
                                                                  NSIndexPath*)>)cellForFn;
@end

namespace aria::adapters::uikit {

template<typename T>
class ObservableTableSource {
public:
    /// Render callback: given a row's `shared_ptr<T>`, the table view
    /// and the index path being asked about, return the
    /// `UITableViewCell*` to display (typically via
    /// `[tableView dequeueReusableCellWithIdentifier:...]`).
    using CellForRowFn = std::function<UITableViewCell*(UITableView*,
                                                        std::shared_ptr<T>,
                                                        NSIndexPath*)>;

    /// Construct a binding between an aria list source and a
    /// `UITableView`. Source writes must be serialized with construction
    /// so snapshot capture and observer registration see one coherent state.
    template<class L>
        requires ::aria::ListSourceOf<L, T>
    ObservableTableSource(UITableView* tableView,
                          L& source,
                          CellForRowFn cell_for_row)
        : state_(std::make_shared<State>()) {
        if (![NSThread isMainThread]) throw std::logic_error("table bridge construction requires the main thread");
        state_->events = std::make_shared<EventQueue>();
        state_->events->target = state_;
        state_->table        = tableView;
        state_->cell_for_row = std::move(cell_for_row);
        state_->snapshot     = source.snapshot();

        std::weak_ptr<State> weak_state = state_;

        auto row_count_fn = [weak_state]() -> NSInteger {
            if (auto s = weak_state.lock(); s && !s->detached.load(std::memory_order_acquire)) {
                return static_cast<NSInteger>(s->snapshot.size());
            }
            return 0;
        };
        auto cell_for_fn = [weak_state](UITableView* tv,
                                        NSIndexPath* indexPath) -> UITableViewCell* {
            auto s = weak_state.lock();
            if (!s || s->detached.load(std::memory_order_acquire)) return nil;
            const NSInteger row = indexPath.row;
            if (row < 0
                || static_cast<std::size_t>(row) >= s->snapshot.size()) {
                return [[UITableViewCell alloc]
                            initWithStyle:UITableViewCellStyleDefault
                          reuseIdentifier:@"empty"];
            }
            auto item = s->snapshot[static_cast<std::size_t>(row)];
            if (!item) {
                return [[UITableViewCell alloc]
                            initWithStyle:UITableViewCellStyleDefault
                          reuseIdentifier:@"empty"];
            }
            return s->cell_for_row(tv, item, indexPath);
        };

        state_->ds = [[AriaUITableDataSource alloc]
                          initWithRowCount:std::move(row_count_fn)
                                 cellForFn:std::move(cell_for_fn)];
        sub_ = source.observe([events = state_->events](const ::aria::ListChange<T>& change) {
            enqueue_(events, change);
        });
        tableView.dataSource = state_->ds;
        tableView.delegate = state_->ds;
        [tableView reloadData];
    }

    ~ObservableTableSource() {
        state_->detached.store(true, std::memory_order_release);
        {
            std::lock_guard lock(state_->events->mutex);
            state_->events->stopped = true;
        }
        sub_.release();
        if ([NSThread isMainThread]) {
            cleanup_(*state_);
        } else {
            // Transfer the sole wrapper owner, not a temporary shared copy:
            // the native data-source and renderer captures are released on main.
            auto* owner = new std::shared_ptr<State>(std::move(state_));
            dispatch_async_f(dispatch_get_main_queue(), owner, [](void* context) {
                std::unique_ptr<std::shared_ptr<State>> state{static_cast<std::shared_ptr<State>*>(context)};
                cleanup_(**state);
            });
        }
    }

    ObservableTableSource(const ObservableTableSource&)            = delete;
    ObservableTableSource& operator=(const ObservableTableSource&) = delete;

    [[nodiscard]] std::size_t row_count() const noexcept {
        return state_ ? state_->snapshot.size() : 0;
    }

    [[nodiscard]] std::shared_ptr<T> at(std::size_t i) const {
        if (!state_) return nullptr;
        if (i >= state_->snapshot.size()) return nullptr;
        return state_->snapshot[i];
    }

private:
    struct State;
    struct EventQueue {
        std::mutex mutex;
        std::deque<::aria::ListChange<T>> changes;
        std::weak_ptr<State> target;
        bool scheduled = false;
        bool stopped = false;
    };
    struct State {
        UITableView* __weak table = nil;
        AriaUITableDataSource* __strong ds = nil;
        CellForRowFn cell_for_row;
        std::vector<std::shared_ptr<T>> snapshot;
        std::atomic<bool> detached{false};
        std::shared_ptr<EventQueue> events;
    };

    static void cleanup_(State& state) {
        std::deque<::aria::ListChange<T>> discarded;
        {
            std::lock_guard lock(state.events->mutex);
            discarded.swap(state.events->changes);
        }
        UITableView* table = state.table;
        if (table.dataSource == state.ds) table.dataSource = nil;
        if (table.delegate == state.ds) table.delegate = nil;
    }

    static void enqueue_(const std::shared_ptr<EventQueue>& events, const ::aria::ListChange<T>& change) {
        {
            std::lock_guard lock(events->mutex);
            if (events->stopped) return;
            events->changes.push_back(change);
            if (events->scheduled) return;
            events->scheduled = true;
        }
        if ([NSThread isMainThread]) drain_(events);
        else {
            auto pending = events; // Copy ownership into the block, not the reference parameter.
            dispatch_async(dispatch_get_main_queue(), ^{ drain_(pending); });
        }
    }

    static void drain_(const std::shared_ptr<EventQueue>& events) {
        auto state = events->target.lock(); // Native State is only retained on main.
        if (!state) return;
        for (;;) {
            ::aria::ListChange<T> change{};
            {
                std::lock_guard lock(events->mutex);
                if (events->stopped || events->changes.empty()) {
                    events->scheduled = false;
                    return;
                }
                change = std::move(events->changes.front());
                events->changes.pop_front();
            }
            if (state->detached.load(std::memory_order_acquire)) return;
            try { apply_change_(*state, change); }
            catch (...) { ::aria::report_callback_failure("uikit.table", std::current_exception()); }
        }
    }

    static NSIndexPath* ip_(std::size_t row) {
        return [NSIndexPath indexPathForRow:static_cast<NSInteger>(row) inSection:0];
    }

    static void apply_change_(State& s,
                              const ::aria::ListChange<T>& ch) {
        using K = ::aria::ListChangeKind;
        switch (ch.kind) {
        case K::Insert:      apply_insert_(s, ch.index, ch.item);      return;
        case K::Remove:      apply_remove_(s, ch.index);                return;
        case K::Replace:     apply_replace_(s, ch.index, ch.item);     return;
        case K::ItemChanged: apply_item_changed_(s, ch.index);          return;
        case K::Move:        apply_move_(s, ch.from_index, ch.index);   return;
        case K::Reset:       apply_reset_(s, ch);                           return;
        }
    }

    static void apply_insert_(State& s,
                              std::size_t idx,
                              const std::shared_ptr<T>& item) {
        if (idx > s.snapshot.size()) idx = s.snapshot.size();
        s.snapshot.insert(s.snapshot.begin() + static_cast<std::ptrdiff_t>(idx),
                          item);
        if (!s.table) return;
        [s.table insertRowsAtIndexPaths:@[ ip_(idx) ]
                       withRowAnimation:UITableViewRowAnimationFade];
    }

    static void apply_remove_(State& s, std::size_t idx) {
        if (idx >= s.snapshot.size()) return;
        s.snapshot.erase(s.snapshot.begin() + static_cast<std::ptrdiff_t>(idx));
        if (!s.table) return;
        [s.table deleteRowsAtIndexPaths:@[ ip_(idx) ]
                       withRowAnimation:UITableViewRowAnimationFade];
    }

    static void apply_replace_(State& s,
                               std::size_t idx,
                               const std::shared_ptr<T>& item) {
        if (idx >= s.snapshot.size()) return;
        s.snapshot[idx] = item;
        if (!s.table) return;
        [s.table reloadRowsAtIndexPaths:@[ ip_(idx) ]
                       withRowAnimation:UITableViewRowAnimationFade];
    }

    static void apply_item_changed_(State& s, std::size_t idx) {
        if (idx >= s.snapshot.size()) return;
        if (!s.table) return;
        [s.table reloadRowsAtIndexPaths:@[ ip_(idx) ]
                       withRowAnimation:UITableViewRowAnimationNone];
    }

    static void apply_move_(State& s, std::size_t from, std::size_t to) {
        if (from == to) return;
        if (from >= s.snapshot.size() || to >= s.snapshot.size()) return;
        auto moved = s.snapshot[from];
        s.snapshot.erase(s.snapshot.begin() + static_cast<std::ptrdiff_t>(from));
        s.snapshot.insert(s.snapshot.begin() + static_cast<std::ptrdiff_t>(to),
                          std::move(moved));
        if (!s.table) return;
        [s.table moveRowAtIndexPath:ip_(from) toIndexPath:ip_(to)];
    }

    static void apply_reset_(State& s, const ::aria::ListChange<T>& change) {
        if (!change.snapshot) throw std::logic_error("Reset requires an owned snapshot");
        s.snapshot = *change.snapshot;
        if (s.table) [s.table reloadData];
    }

    std::shared_ptr<State> state_;
    ::aria::Subscription   sub_;
};

}  // namespace aria::adapters::uikit
