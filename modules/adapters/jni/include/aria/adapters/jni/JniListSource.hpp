#pragma once

// JniListSource.hpp — bridge any aria list source onto a Kotlin/Java
// `RecyclerView.Adapter`.
//
// JNI counterpart of `qt_list_model_adapter.hpp`, `UIKitTableSource.hpp`
// and `AppKitTableSource.hpp`. Accepts any source satisfying
// `aria::ListSourceOf<L, T>` (ObservableList / FilteredList / SortedList
// / MappedList / DistinctList / PagedList / GroupedList) and turns
// `ListChange<T>` events into the RecyclerView notification vocabulary:
//
//     Insert       -> notifyItemInserted(position)
//     Remove       -> notifyItemRemoved(position)
//     Replace      -> notifyItemChanged(position)
//     ItemChanged  -> notifyItemChanged(position)
//     Move         -> notifyItemMoved(fromPosition, toPosition)
//     Reset        -> notifyDataSetChanged()
//
// Why this exists
// ---------------
// Without it the documented workaround was to join list items with "\n"
// on the C++ side and split them back in Kotlin, which discards item
// identity, per-row diffing and selection. This bridge keeps the same
// per-row diffing the other three adapters already provide.
//
// How it differs from the other three adapters — and why
// ------------------------------------------------------
// Qt / UIKit / AppKit all hold a pointer to the *native widget* and call
// into it. RecyclerView.Adapter lives on the managed side and cannot be
// held that way: notifications are `protected`-ish API on the adapter
// instance, so the C++ side must call *back into Java*. That inversion is
// why the notification target is expressed as a `NotifySink` callable
// rather than a widget handle:
//
//   * `JniListSource<T>` owns the snapshot and the diffing. It has no
//     `<jni.h>` dependency, so the diff logic is host-testable without a
//     JVM — the JNI adapter's other tests can only static_assert, and a
//     list bridge whose diffing nobody can run is how off-by-one row
//     bugs ship.
//   * `jni_notify_sink(...)` (below, JNI-only) builds the sink that
//     performs the actual `CallVoidMethod` calls.
//
// Threading
// ---------
// RecyclerView mutation MUST happen on the Android main (Looper) thread.
// This class does NOT hop threads by itself — Aria has no Android looper
// abstraction, and inventing one here would exceed an adapter's remit.
// Instead the sink is invoked on whatever thread emitted the change, and
// the caller is expected to install a sink that posts to the main looper
// (`Handler`/`View::post`) when its producer can emit off-main. The
// `shared_ptr<T>` for events that need it is resolved at emit time,
// matching the Qt / UIKit / AppKit contract, so a sink that defers still
// sees the item that was live when the change was announced.
//
// Lifetime
// --------
// The bridge holds a non-owning reference to the source list (the caller
// keeps it alive) and drops its `Subscription` first on destruction, so no
// notification can reach a half-destroyed sink.

#include "aria/list_source.hpp"
#include "aria/observable_list.hpp"
#include "aria/subscription.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace aria::adapters::jni {

/// Which RecyclerView notification to raise. Mirrors `ListChangeKind`
/// but names the *managed* call, because that is what a sink implements.
enum class RecyclerNotify {
    ItemInserted,      ///< notifyItemInserted(position)
    ItemRemoved,       ///< notifyItemRemoved(position)
    ItemChanged,       ///< notifyItemChanged(position)
    ItemMoved,         ///< notifyItemMoved(from, to)
    DataSetChanged,    ///< notifyDataSetChanged()
};

/// One notification to forward to the managed adapter. `position` is the
/// affected row (the destination row for `ItemMoved`); `from_position` is
/// meaningful for `ItemMoved` only.
struct RecyclerNotification {
    RecyclerNotify kind = RecyclerNotify::DataSetChanged;
    std::size_t    position = 0;
    std::size_t    from_position = 0;
};

/// Bridges an aria list source to a RecyclerView.Adapter.
///
/// `T` is the element type; rows are `std::shared_ptr<T>` exactly as in
/// the other adapters, so item identity survives the hop.
template<typename T>
class JniListSource {
public:
    /// Receives each notification that must be forwarded to the managed
    /// `RecyclerView.Adapter`. Use `jni_notify_sink()` for the standard
    /// JNI implementation, or a lambda in tests.
    using NotifySink = std::function<void(const RecyclerNotification&)>;

    /// Bind `source` to `sink`. The source must outlive `*this`.
    ///
    /// The initial snapshot is taken before observation starts, so
    /// `item_count()` is correct before the first notification and the
    /// managed adapter can be attached in either order.
    template<class L>
        requires ::aria::ListSourceOf<L, T>
    JniListSource(L& source, NotifySink sink)
        : sink_(std::move(sink)),
          snapshot_(source.snapshot()),
          size_fn_([&source] { return source.size(); }),
          at_fn_([&source](std::size_t i) { return source.at(i); }) {
        sub_ = source.observe([this](const ::aria::ListChange<T>& ch) {
            // Resolve the item WHILE THE CHANGE IS FRESH. A sink that
            // defers to the main looper would otherwise re-read the
            // source after further mutations and hand the wrong row to
            // the managed side (same rationale as the Qt adapter).
            std::shared_ptr<T> resolved;
            using K = ::aria::ListChangeKind;
            if (ch.kind == K::Insert
                || ch.kind == K::Replace
                || ch.kind == K::Move) {
                if (ch.index < size_fn_()) {
                    resolved = at_fn_(ch.index);
                }
            }
            apply_change_(ch, std::move(resolved));
        });
    }

    ~JniListSource() {
        // Detach first: no notification may reach a sink whose captured
        // state is already being torn down.
        sub_ = ::aria::Subscription{};
    }

    JniListSource(const JniListSource&)            = delete;
    JniListSource& operator=(const JniListSource&) = delete;

    /// Row count for `RecyclerView.Adapter.getItemCount()`.
    [[nodiscard]] std::size_t item_count() const {
        std::lock_guard lk(mutex_);
        return snapshot_.size();
    }

    /// Row payload for `onBindViewHolder(holder, position)`. Returns
    /// nullptr for an out-of-range position rather than throwing: the
    /// managed side may ask about a row that a pending notification has
    /// already removed.
    [[nodiscard]] std::shared_ptr<T> at(std::size_t position) const {
        std::lock_guard lk(mutex_);
        if (position >= snapshot_.size()) return nullptr;
        return snapshot_[position];
    }

    /// Rebuild from the source and raise one `notifyDataSetChanged()`.
    /// The escape hatch for a managed adapter that lost sync (e.g. it was
    /// re-attached after a configuration change).
    void reload() {
        {
            std::lock_guard lk(mutex_);
            rebuild_locked_();
        }
        notify_({RecyclerNotify::DataSetChanged, 0, 0});
    }

private:
    void rebuild_locked_() {
        const auto n = size_fn_();
        snapshot_.clear();
        snapshot_.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            snapshot_.push_back(at_fn_(i));
        }
    }

    /// Invoke the sink with the mutex released. A managed notification
    /// can re-enter this object through `getItemCount` / `onBind`
    /// (RecyclerView queries the adapter synchronously inside
    /// `notifyItem*`), which would self-deadlock under the lock — the
    /// same hazard as the adapters that destroy cached views under their
    /// own mutex (lifecycle.md A11).
    void notify_(const RecyclerNotification& n) const {
        if (sink_) sink_(n);
    }

    void apply_change_(const ::aria::ListChange<T>& ch,
                       std::shared_ptr<T> resolved) {
        using K = ::aria::ListChangeKind;

        // Decide the notification while holding the lock (snapshot must
        // move atomically with respect to readers), then raise it after
        // releasing — see notify_().
        RecyclerNotification out;
        bool                 should_notify = false;
        {
            std::lock_guard lk(mutex_);
            switch (ch.kind) {
            case K::Insert: {
                auto idx = ch.index;
                if (idx > snapshot_.size()) idx = snapshot_.size();
                snapshot_.insert(
                    snapshot_.begin() + static_cast<std::ptrdiff_t>(idx),
                    std::move(resolved));
                out = {RecyclerNotify::ItemInserted, idx, 0};
                should_notify = true;
                break;
            }
            case K::Remove: {
                if (ch.index >= snapshot_.size()) break;
                snapshot_.erase(
                    snapshot_.begin() + static_cast<std::ptrdiff_t>(ch.index));
                out = {RecyclerNotify::ItemRemoved, ch.index, 0};
                should_notify = true;
                break;
            }
            case K::Replace: {
                if (ch.index >= snapshot_.size()) break;
                snapshot_[ch.index] = std::move(resolved);
                out = {RecyclerNotify::ItemChanged, ch.index, 0};
                should_notify = true;
                break;
            }
            case K::ItemChanged: {
                // T's own on_changed fired; the row object is unchanged,
                // so only a re-bind is needed.
                if (ch.index >= snapshot_.size()) break;
                out = {RecyclerNotify::ItemChanged, ch.index, 0};
                should_notify = true;
                break;
            }
            case K::Move: {
                if (ch.from_index >= snapshot_.size()
                    || ch.index >= snapshot_.size()
                    || ch.from_index == ch.index) break;
                auto moved = snapshot_[ch.from_index];
                snapshot_.erase(snapshot_.begin()
                                + static_cast<std::ptrdiff_t>(ch.from_index));
                snapshot_.insert(snapshot_.begin()
                                     + static_cast<std::ptrdiff_t>(ch.index),
                                 std::move(moved));
                // RecyclerView's notifyItemMoved takes the raw (from, to)
                // pair — unlike Qt's beginMoveRows, it needs no +1
                // adjustment for downward moves.
                out = {RecyclerNotify::ItemMoved, ch.index, ch.from_index};
                should_notify = true;
                break;
            }
            case K::Reset: {
                // Matches ObservableList::clear — the list is now empty
                // and repopulation arrives as subsequent Inserts.
                rebuild_locked_();
                out = {RecyclerNotify::DataSetChanged, 0, 0};
                should_notify = true;
                break;
            }
            }
        }
        if (should_notify) notify_(out);
    }

    mutable std::mutex               mutex_;
    NotifySink                       sink_;
    std::vector<std::shared_ptr<T>>  snapshot_;
    std::function<std::size_t()>                    size_fn_;
    std::function<std::shared_ptr<T>(std::size_t)>  at_fn_;
    ::aria::Subscription             sub_;
};

}  // namespace aria::adapters::jni
