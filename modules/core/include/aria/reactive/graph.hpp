#pragma once

// ============================================================================
//  reactive/graph.hpp
// ----------------------------------------------------------------------------
//  `Graph` is the process-wide coordinator of the reactive subsystem
//  (one instance per process, reached via `Node::graph()`).
//
//  The Graph provides four things:
//    1. Transactions (batches). Multiple Property writes coalesce into a
//       single flush, notifying the UI exactly once -- eliminating the
//       "3 setters -> 3 repaints" problem entirely.
//    2. Push phase (coloring). A source change propagates MaybeDirty down
//       the DAG without triggering any computation.
//    3. Pull phase (evaluation). At flush time the affected nodes are
//       evaluated in topological order (ascending depth); nodes whose
//       upstreams did not actually move are skipped, which is what makes
//       the graph glitch-free.
//    4. Tracking context. A Derivation declares "I read from X" during
//       compute via the explicit `dep(x)` API, which the Graph turns into
//       an Edge. The API is explicit on purpose -- no hidden subscriptions,
//       no accidental "I just logged a value and now I depend on it".
//
//  Threading model
//  ---------------
//  Single-threaded (UI thread). Every public Graph entry point asserts the
//  calling thread in Debug builds. Cross-thread updates must be posted via
//  the Dispatcher to the graph thread before invoking any API here.
//
//  Cycles
//  ------
//  If flush encounters a dependency cycle B -> ... -> B, it raises
//  `CircularDependencyError` instead of spinning or overflowing the stack.
// ============================================================================

#include "aria/reactive/node.hpp"
#include "aria/callback_boundary.hpp"

#include <algorithm>
#include <cassert>
#include <exception>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace aria::reactive {

/// Thrown when flush detects a dependency cycle. `what()` contains the
/// debug_name chain of the nodes involved.
class CircularDependencyError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// ---------------------------------------------------------------------------
//  Flush tracing hook (used by `aria::GraphInspector`).
//  We keep the type minimal here so `graph.hpp` does not have to know
//  anything about the inspector; `inspector.hpp` packages these raw
//  parameters into a nicer `FlushEvent` struct for user callbacks.
//
//  Phase codes are stable integers so the ABI of the hook never breaks
//  across versions:
//    0 = FlushBegin   (node = nullptr)
//    1 = RoundBegin   (node = nullptr,  round = 1..N)
//    2 = Pull         (node = n,         round = 1..N)
//    3 = SkipClean    (node = n,         round = 1..N)
//    4 = Recomputed   (node = n,         round, changed)
//    5 = RoundEnd     (node = nullptr,  round)
//    6 = FlushEnd     (node = nullptr)
// ---------------------------------------------------------------------------
using FlushTraceFn = std::function<void(int phase,
                                        const Node* node,
                                        int round,
                                        bool changed)>;

inline FlushTraceFn& flush_trace_hook_() {
    static FlushTraceFn hook;
    return hook;
}

/// One "upstream read" record, produced each time a Derivation calls
/// `dep(source)` during a compute. The Graph's TrackingContext collects
/// them and reconciles the edge set once compute finishes.
struct ReadRecord {
    Node* source;  ///< The upstream node that was read.
};

/// Per-recompute tracking context for a single Derivation evaluation.
class TrackingContext {
public:
    /// Explicit dependency injection point, invoked from `dep(prop)`.
    /// Supplied by `reactive::current_tracker()`.
    void record_read(Node& src) {
        // Small read sets -- linear de-dup is more than fast enough.
        for (Node* n : reads_) {
            if (n == &src) return;
        }
        reads_.push_back(&src);
    }

    [[nodiscard]] const std::vector<Node*>& reads() const noexcept { return reads_; }
    void                                     clear() noexcept      { reads_.clear(); }

private:
    std::vector<Node*> reads_;
};

/// Process-wide singleton reactive graph (accessed via `Node::graph()`).
class Graph {
    // `Node::mark_downstream_maybe_dirty()` reaches into `color_stack_`
    // and `color_in_use_` to amortise the coloring buffer across
    // notifications. Symmetric to `Node`'s existing `friend class
    // Graph;` declaration — together they keep the intrusive coupling
    // between the two header-only types contained.
    friend class Node;

public:
    Graph() noexcept;
    ~Graph() noexcept = default;

    Graph(const Graph&)            = delete;
    Graph& operator=(const Graph&) = delete;

    // ---- Debug thread assertion (no-op in Release) -----------------------
    void assert_on_graph_thread() const noexcept {
#ifndef NDEBUG
        // First call records the thread; later calls must match.
        if (owner_thread_ == std::thread::id{}) {
            const_cast<Graph*>(this)->owner_thread_ = std::this_thread::get_id();
            return;
        }
        assert(owner_thread_ == std::this_thread::get_id()
               && "reactive::Graph accessed from a non-UI thread. "
                  "Use Dispatcher::post to marshal updates back to the graph thread.");
#endif
    }

    // ------------------------------------------------------------------
    //  Transactions (batches)
    // ------------------------------------------------------------------

    /// Open a new batch. Nesting is legal; the outermost close flushes.
    void begin_batch() noexcept {
        assert_on_graph_thread();
        ++batch_depth_;
    }

    /// Close the current batch; triggers a flush once the outermost batch
    /// closes (unless we are already inside a flush).
    void end_batch() {
        assert_on_graph_thread();
        assert(batch_depth_ > 0 && "end_batch without matching begin_batch");
        if (--batch_depth_ == 0 && !flushing_) {
            flush();
        }
    }

    [[nodiscard]] bool in_batch() const noexcept { return batch_depth_ > 0; }

    // ------------------------------------------------------------------
    //  Push phase -- invoked when a Source's value changes.
    // ------------------------------------------------------------------

    /// Called after a Source has committed a new value: bumps its version
    /// and colors the downstream MaybeDirty.
    /// - Inside a batch: only colors, no flush.
    /// - Outside a batch: a single-shot flush runs immediately.
    void on_source_changed(Node& src);

    /// Enqueue a node into the current flush round's "pending" set.
    /// Typical use: a Reaction node has no value but must still run on
    /// flush.
    void enqueue_dirty(Node& n) {
        pending_.push_back(&n);
    }

    // ------------------------------------------------------------------
    //  Pull phase -- flush and evaluation
    // ------------------------------------------------------------------

    /// Evaluate every dirty node in topological order. Nodes whose
    /// recompute() changes the cached value seed a new round by
    /// re-coloring their downstream. The loop runs at most
    /// `kMaxFlushRounds` rounds before declaring a cycle.
    void flush();

    /// Force-evaluate a single node if it is Dirty / MaybeDirty.
    /// Returns true iff the node's version was advanced by this call.
    bool pull(Node& n);

    // ------------------------------------------------------------------
    //  Tracking
    // ------------------------------------------------------------------

    [[nodiscard]] TrackingContext* current_tracker() noexcept {
        return tracker_stack_.empty() ? nullptr : tracker_stack_.back();
    }

    /// Push / pop a tracker (used by RAII TrackerScope).
    void push_tracker(TrackingContext* ctx) { tracker_stack_.push_back(ctx); }
    void pop_tracker(TrackingContext* ctx) {
        assert(!tracker_stack_.empty() && tracker_stack_.back() == ctx);
        (void)ctx;
        tracker_stack_.pop_back();
    }

    /// Enter / leave an untracked scope. Implemented as pushing a nullptr
    /// tracker so `current_tracker()` returns null inside, which makes
    /// `dep()` behave like a plain read.
    void enter_untracked() { tracker_stack_.push_back(nullptr); }
    void leave_untracked() {
        assert(!tracker_stack_.empty() && tracker_stack_.back() == nullptr);
        tracker_stack_.pop_back();
    }

private:
    // Inner helper used by `pull`: assumes every MaybeDirty upstream of
    // `n` has already been settled, and runs the version-compare /
    // recompute leg without recursing into upstream resolution. The
    // public `pull` first walks the graph iteratively to reach this
    // precondition, then calls `pull_settle_` for each node bottom-up.
    bool pull_settle_(Node& n);

    // Reusable scratch buffer for `Node::mark_downstream_maybe_dirty()`.
    // The coloring walk used to allocate a fresh `std::vector<Node*>`
    // every time a Source committed a value — which is millions of
    // allocations per second on hot UI workloads. Hoisting it into the
    // (single-threaded) graph lets us amortise the buffer across every
    // notify, capacity grows monotonically to the deepest fan-out
    // observed and is reused thereafter. Exposed only to `Node` via the
    // existing `friend class Graph` declaration.
    std::vector<Node*> color_stack_;
    bool color_in_use_ = false;   // re-entrancy guard

    // Guard against nested flush calls. Recursive set() re-enqueues into
    // the next round instead of starting a new flush.
    bool flushing_ = false;

    int batch_depth_ = 0;

    // Nodes colored dirty in the current round. De-duplication is
    // implicitly handled by `state_` (Clean entries skipped).
    std::vector<Node*> pending_;

    // Active recursive `pull()` stack. When a cycle trips the
    // `Computing` re-entry check we use this to format a full
    // A → B → C → A path in the error message — vastly more useful
    // than just "node X is re-entering itself".
    std::vector<Node*> pulling_stack_;

    // Tracker stack. `nullptr` entries denote untracked scopes.
    std::vector<TrackingContext*> tracker_stack_;

    std::thread::id owner_thread_{};

    // Safety fuse against runaway propagation (which should only happen
    // when there is a bug in a user-supplied recompute function).
    static constexpr int kMaxFlushRounds = 100;
};

// ------------------------------------------------------------------
//  Convenience API -- user-facing entry points
// ------------------------------------------------------------------

/// Returns the current tracker of the global graph. Used internally by
/// Property / AutoComputed.
[[nodiscard]] inline TrackingContext* current_tracker() noexcept {
    return Node::graph().current_tracker();
}

/// RAII: push a tracker on construction, pop on destruction.
class TrackerScope {
public:
    explicit TrackerScope(TrackingContext& ctx) : ctx_(&ctx) {
        Node::graph().push_tracker(&ctx);
    }
    ~TrackerScope() { Node::graph().pop_tracker(ctx_); }
    TrackerScope(const TrackerScope&)            = delete;
    TrackerScope& operator=(const TrackerScope&) = delete;
private:
    TrackingContext* ctx_;
};

/// RAII: within the scope, every `dep()` behaves like a plain get().
class UntrackedScope {
public:
    UntrackedScope() { Node::graph().enter_untracked(); }
    ~UntrackedScope() { Node::graph().leave_untracked(); }
    UntrackedScope(const UntrackedScope&)            = delete;
    UntrackedScope& operator=(const UntrackedScope&) = delete;
};

/// Sugar: `untracked([&]{ ... })`. Matches the MobX/SolidJS naming so
/// users coming from those ecosystems feel at home.
template<class Fn>
auto untracked(Fn&& fn) -> decltype(fn()) {
    UntrackedScope guard;
    if constexpr (std::is_void_v<decltype(fn())>) {
        std::forward<Fn>(fn)();
    } else {
        return std::forward<Fn>(fn)();
    }
}

/// RAII batch guard: `{ BatchScope b; ...; }` or use `batch([&]{...})`.
///
/// If `flush()` throws (e.g. a `CircularDependencyError`), the destructor
/// catches it and reports through the diagnostic sink rather than letting
/// the exception escape — destructors are implicitly `noexcept` and any
/// exception escaping during stack unwinding from a user error would
/// terminate the process. The user-visible `flush()` call from a
/// `batch([&]{...})` body still throws normally; only the dtor swallows.
class BatchScope {
public:
    BatchScope() { Node::graph().begin_batch(); }
    ~BatchScope() noexcept {
        try {
            Node::graph().end_batch();
        } catch (...) {
            // Avoid std::terminate during unwinding, but do not make
            // the failure invisible: route it through the lightweight
            // callback-failure sink (stderr fallback until the host
            // installs its own sink). Keep this dependency in core so
            // the header remains portable across GCC/Clang/MSVC.
            ::aria::report_callback_failure(
                std::string_view{"reactive.batch_scope.end_batch"},
                std::current_exception());
        }
    }
    BatchScope(const BatchScope&)            = delete;
    BatchScope& operator=(const BatchScope&) = delete;
};

/// Sugar: `batch([&]{ firstName = "..."; lastName = "..."; })`.
template<class Fn>
auto batch(Fn&& fn) -> decltype(fn()) {
    BatchScope guard;
    if constexpr (std::is_void_v<decltype(fn())>) {
        std::forward<Fn>(fn)();
    } else {
        return std::forward<Fn>(fn)();
    }
}

}  // namespace aria::reactive
