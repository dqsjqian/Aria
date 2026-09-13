#pragma once

// ============================================================================
//  reactive/graph.inl
// ----------------------------------------------------------------------------
//  Inline definitions of `Graph` and `Node` methods that depend on each
//  other's complete types.
//
//  `core` is a header-only INTERFACE library, so these implementations live
//  in a .inl included from a single public header. Every function is marked
//  `inline` to avoid ODR violations across translation units.
// ============================================================================

#include "aria/reactive/graph.hpp"
#include "aria/reactive/node.hpp"

#include "aria/diagnostics.hpp"

#include <algorithm>
#include <cassert>

namespace aria::reactive {

// ---------------------------------------------------------------------------
//  The graph storage lives in aria_abi, so every shared module observes the
//  same tracker and flush state. Template operations remain inline here.
// ---------------------------------------------------------------------------
ARIA_ABI_API Graph& graph_instance() noexcept;

inline Graph& Node::graph() noexcept { return graph_instance(); }

inline Graph::Graph() noexcept = default;

// ---------------------------------------------------------------------------
//  Node destructor: must detach from every incident edge, otherwise we
//  leave dangling Edge records pointing at freed memory -> UAF.
// ---------------------------------------------------------------------------
inline Node::~Node() noexcept {
    retire_();
}

inline void Node::retire_() noexcept {
    // Clear borrowed references before any member/capture destructor can
    // re-enter the graph. No allocation and no graph-wide queue scan.
    while (handles_head_) {
        auto* handle = handles_head_;
        handles_head_ = handle->next_;
        handle->node_ = nullptr;
        handle->previous_ = handle->next_ = nullptr;
    }
    // Detach downstream observers (their Edge::source would dangle).
    while (observers_head_ != nullptr) {
        Edge* e = observers_head_;
        // detach_edge also removes `e` from source->observers_head_ below.
        e->observer->detach_edge(*e);
    }
    // Detach our own subscriptions to upstream nodes.
    clear_sources();
}

// ---------------------------------------------------------------------------
//  Node: intrusive linked-list manipulation of upstream / downstream edges.
// ---------------------------------------------------------------------------
inline void Node::attach_as_observer_of(Node& source, Edge& edge) noexcept {
    edge.source           = &source;
    edge.observer         = this;
    edge.observed_version = source.version_;

    // Insert at the head of source's observers list.
    edge.prev_observer = nullptr;
    edge.next_observer = source.observers_head_;
    if (source.observers_head_) source.observers_head_->prev_observer = &edge;
    source.observers_head_ = &edge;

    // Insert at the head of our own sources list.
    edge.prev_source = nullptr;
    edge.next_source = sources_head_;
    if (sources_head_) sources_head_->prev_source = &edge;
    sources_head_ = &edge;

    // Maintain topological depth (observer depth >= source depth + 1).
    // Used by flush to sort pending nodes ascending.
    if (source.depth_ + 1 > depth_) {
        depth_ = source.depth_ + 1;
    }
}

inline void Node::detach_edge(Edge& edge) noexcept {
    // Unlink from source->observers list.
    Node* src = edge.source;
    if (src != nullptr) {
        if (edge.prev_observer) edge.prev_observer->next_observer = edge.next_observer;
        else                    src->observers_head_              = edge.next_observer;
        if (edge.next_observer) edge.next_observer->prev_observer = edge.prev_observer;
    }
    // Unlink from our sources list.
    if (edge.prev_source) edge.prev_source->next_source = edge.next_source;
    else                  sources_head_                 = edge.next_source;
    if (edge.next_source) edge.next_source->prev_source = edge.prev_source;

    edge.source        = nullptr;
    edge.observer      = nullptr;
    edge.next_observer = edge.prev_observer = nullptr;
    edge.next_source   = edge.prev_source   = nullptr;
}

inline void Node::clear_sources() noexcept {
    while (sources_head_ != nullptr) {
        detach_edge(*sources_head_);
    }
}

// ---------------------------------------------------------------------------
//  Node: a Source signals a value change. Bumps version, colors downstream,
//  and hands off to the Graph to decide whether to flush immediately.
// ---------------------------------------------------------------------------
inline void Node::notify_changed() {
    Graph& g = graph();
    g.assert_on_graph_thread();
    ++version_;
    // Sources are authoritative, so downstream starts as MaybeDirty and
    // will be promoted to Dirty by Graph::pull after comparing versions.
    mark_downstream_maybe_dirty();
    g.on_source_changed(*this);
}

inline void Node::mark_downstream_maybe_dirty() {
    // Iterative coloring using an explicit stack to avoid blowing the
    // C++ call stack on very deep DAGs.
    //
    // The stack lives on `Graph` so we don't allocate a fresh vector on
    // every Source commit. Re-entry (e.g. a Reaction whose body itself
    // commits another Source while a coloring walk is mid-flight) falls
    // back to a local buffer to keep the outer walk's state intact.
    Graph& g = graph();
    std::vector<Node*> local_storage;
    std::vector<Node*>* stack = nullptr;
    bool owns_global = false;
    if (!g.color_in_use_) {
        g.color_in_use_ = true;
        owns_global = true;
        stack = &g.color_stack_;
        stack->clear();
    } else {
        stack = &local_storage;
        local_storage.reserve(8);
    }
    // RAII to release the global slot even if a downstream observer-list
    // walk somehow throws (it shouldn't — only state writes happen).
    struct Release {
        Graph* g; bool owned;
        ~Release() { if (owned) g->color_in_use_ = false; }
    } release{&g, owns_global};

    stack->push_back(this);
    while (!stack->empty()) {
        Node* cur = stack->back();
        stack->pop_back();

        for (Edge* e = cur->observers_head_; e != nullptr; e = e->next_observer) {
            Node* obs = e->observer;
            if (obs->state_ == NodeState::Clean) {
                obs->state_ = NodeState::MaybeDirty;
                stack->push_back(obs);
            }
        }
    }
}

// ---------------------------------------------------------------------------
//  Graph: source-change event. Enqueue direct downstream nodes; deeper
//  descendants will surface naturally when their parents report a real
//  value change during flush.
// ---------------------------------------------------------------------------
inline void Graph::on_source_changed(Node& src) {
    for (Edge* e = src.observers_head_; e != nullptr; e = e->next_observer) {
        enqueue_dirty(*e->observer);
    }
    if (batch_depth_ == 0 && !flushing_) {
        flush();
    }
}

// ---------------------------------------------------------------------------
//  Graph::flush -- topologically-ordered pull, the core glitch-free loop.
//
//  Algorithm (the Pull half of Push-Pull):
//    1. Snapshot `pending_`; sort by ascending depth (stable).
//    2. Pull each node in that order. If the recompute produces a changed
//       value, its downstream is re-colored and enqueued for the next round.
//    3. Repeat until `pending_` is empty.
//    4. If the round count exceeds `kMaxFlushRounds`, declare a cycle.
// ---------------------------------------------------------------------------
inline void Graph::flush() {
    assert_on_graph_thread();
    if (flushing_) return;  // no nested flush; recursive set() rolls over
    flushing_ = true;

    // Install before copying or calling diagnostics: either can throw.
    struct Guard {
        Graph* g;
        ~Guard() {
            g->round_.clear();
            g->flushing_ = false;
        }
    } guard{this};

    int rounds = 0;

    // Flush-tracing hook (installed by `GraphInspector`). The empty
    // `std::function` check is a single null-compare, so this line costs
    // essentially nothing when diagnostics are not enabled.
    const auto trace = flush_trace_hook_();
    const auto trace_phase = [&](int phase, const Node* node, int round, bool changed) {
        if (!trace) return;
        try {
            (*trace)(phase, node, round, changed);
        } catch (...) {
            ::aria::report_callback_failure("reactive.flush_tracer", std::current_exception());
        }
    };

    // Bridge to the unified diagnostic sink. Same zero-cost
    // contract: when no sink is installed, `has_trace_sink()` is one
    // atomic load + null check.
    auto publish_phase = [](::aria::trace::ReactivePhase phase,
                            const Node* node, int round, bool changed) {
        if (!::aria::has_trace_sink()) return;
        ::aria::trace::Reactive payload{
            phase,
            node ? node->effective_debug_name() : std::string{},
            round,
            changed,
        };
        ::aria::publish_trace_unchecked(::aria::TraceCategory::Reactive,
                              std::move(payload));
    };

    trace_phase(0 /*FlushBegin*/, nullptr, 0, false);
    publish_phase(::aria::trace::ReactivePhase::FlushBegin, nullptr, 0, false);

    // A diagnostic callback may retire its node. Re-read the borrowed
    // handle after each callback, including the unified trace sink.
    auto trace_node = [&](int phase, ::aria::trace::ReactivePhase category,
                          const detail::NodeHandle& node, bool changed) {
        if (!node) return;
        trace_phase(phase, node.get(), rounds, changed);
        if (node) publish_phase(category, node.get(), rounds, changed);
    };

    while (!pending_.empty()) {
        if (++rounds > kMaxFlushRounds) {
            // Capture a representative cycle path: the remaining pending
            // nodes sorted by depth form a superset of any ongoing cycle.
            // We list their debug names (or addresses as a fallback) so
            // the caller can pinpoint the loop with no inspector needed.
            std::string detail = "reactive::Graph::flush exceeded "
                                 + std::to_string(kMaxFlushRounds)
                                 + " rounds -- likely a circular dependency. "
                                 + "Pending nodes still dirty:";
            int printed = 0;
            for (const auto& n : pending_) {
                if (!n) continue;
                if (printed++ >= 16) { detail += " ..."; break; }
                detail += "\n  - ";
                detail += n->effective_debug_name();
            }
            for (const auto& n : pending_) if (n) n->queued_ = false;
            pending_.clear();
            throw CircularDependencyError(detail);
        }

        // Snapshot the current round; new entries during pull land in the
        // fresh empty `pending_` and will be processed in the next round.
        round_.swap(pending_);
        for (const auto& n : round_) if (n) n->queued_ = false;

        // Sort by depth so parents resolve before children (the essence
        // of glitch-free evaluation), then de-duplicate.
        std::sort(round_.begin(), round_.end(),
                  [](const auto& a, const auto& b) {
                      if (!a || !b) return !a && static_cast<bool>(b);
                      if (a->depth() != b->depth()) return a->depth() < b->depth();
                      return a->node_id_ < b->node_id_;
                  });

        trace_phase(1 /*RoundBegin*/, nullptr, rounds, false);
        publish_phase(::aria::trace::ReactivePhase::RoundBegin, nullptr, rounds, false);

        for (const auto& n : round_) {
            if (!n) continue;
            // A node may have been pulled by an earlier entry in this
            // same round (via the upstream recursion inside pull()),
            // reaching Clean state. Skip those.
            if (n->state() == NodeState::Clean) {
                trace_node(3, ::aria::trace::ReactivePhase::SkipClean, n, false);
                continue;
            }
            trace_node(2, ::aria::trace::ReactivePhase::Pull, n, false);
            if (!n) continue;
            const bool changed = pull(*n);
            trace_node(4, ::aria::trace::ReactivePhase::Recomputed, n, changed);
        }

        trace_phase(5 /*RoundEnd*/, nullptr, rounds, false);
        publish_phase(::aria::trace::ReactivePhase::RoundEnd, nullptr, rounds, false);
        round_.clear();
    }

    trace_phase(6 /*FlushEnd*/, nullptr, rounds, false);
    publish_phase(::aria::trace::ReactivePhase::FlushEnd, nullptr, rounds, false);
}

// ---------------------------------------------------------------------------
//  Graph::pull -- evaluate a single node on demand; decide whether to
//  propagate downstream based on whether the value actually changed.
//
//  Algorithm:
//    1. If Clean -> nothing to do.
//    2. If MaybeDirty -> for each upstream, compare versions.
//         - No upstream actually moved -> mark Clean, return false.
//         - At least one moved          -> promote to Dirty.
//    3. If Dirty -> invoke recompute(). A true return indicates the
//         cached value changed, which:
//           a) enqueues direct downstream nodes for the next round;
//           b) leaves `version_` already advanced by recompute().
//    4. State returns to Clean; `changed` propagates back to the caller.
//
//  Stack discipline:
//    The MaybeDirty fast-path used to recursively call `pull(*upstream)`
//    on each non-Clean source. On very deep DAGs (thousands of chained
//    Computeds) that recursion could blow the C++ call stack — only the
//    push side used an explicit worklist. We now replicate the same
//    iterative pattern on the pull side: walk MaybeDirty ancestors with
//    an explicit stack, settling them bottom-up, then come back to `n`
//    knowing every upstream is Clean. The recursion-only piece left in
//    the loop is the call to `recompute()`, which itself runs user code
//    that may legitimately read other Computeds (those calls re-enter
//    `pull` once per chain link, but at the height of a *user* read
//    chain, not the height of the whole DAG).
// ---------------------------------------------------------------------------
inline bool Graph::pull(Node& n) {
    if (n.state() == NodeState::Clean) return false;
    if (n.state() == NodeState::Computing) return pull_settle_(n);

    bool has_unsettled_parent = false;
    n.for_each_source([&](const Edge& e) {
        has_unsettled_parent |= e.source->state() == NodeState::MaybeDirty;
    });
    if (!has_unsettled_parent) return pull_settle_(n);

    // Only a pull through unresolved ancestors needs a worklist. Each
    // entered frame marks the active DFS path, so MaybeDirty cycles are
    // diagnosed before the worklist can grow without bound.
    struct Frame {
        explicit Frame(Node* target) noexcept : node(target) {}

        detail::NodeHandle node;
        bool entered = false;
    };
    std::vector<Frame> work;
    struct VisitGuard {
        std::vector<Frame>& work;
        ~VisitGuard() {
            for (const auto& frame : work) {
                if (frame.node && frame.entered) frame.node->resolving_ = false;
            }
        }
    } visit_guard{work};
    work.emplace_back(&n);
    while (!work.empty()) {
        if (!work.back().node) { work.pop_back(); continue; }
        auto& frame = work.back();
        Node& current = *frame.node;
        if (!frame.entered) {
            if (current.resolving_) {
                std::string path;
                for (const auto& entry : work) {
                    if (!entry.node) continue;
                    if (!path.empty()) path += " -> ";
                    path += entry.node->effective_debug_name();
                }
                throw CircularDependencyError(
                    "reactive::Graph::pull detected an unresolved dependency cycle: " + path);
            }
            current.resolving_ = true;
            frame.entered = true;
        }

        Node* unresolved = nullptr;
        current.for_each_source([&](const Edge& e) {
            if (!unresolved && e.source->state() == NodeState::MaybeDirty) {
                unresolved = e.source;
            }
        });
        if (unresolved) {
            work.emplace_back(unresolved);
            continue;
        }

        auto ready = std::move(frame.node);
        current.resolving_ = false;
        work.pop_back();
        const bool changed = pull_settle_(*ready);
        if (work.empty()) return changed;
    }
    return false; // The requested node was retired by an upstream callback.
}

// ---------------------------------------------------------------------------
//  Graph::pull_settle_ -- the recompute leg of pull(). Assumes every
//  MaybeDirty upstream has already been settled (Clean or Dirty with
//  a definitive version), so the version-comparison fast-path is valid.
// ---------------------------------------------------------------------------
inline bool Graph::pull_settle_(Node& n) {
    if (n.state() == NodeState::Clean) return false;

    // Cycle detection: re-entering an already-Computing node = cycle.
    if (n.state() == NodeState::Computing) {
        std::string path;
        std::size_t cycle_start = pulling_stack_.size();
        for (std::size_t i = 0; i < pulling_stack_.size(); ++i) {
            if (pulling_stack_[i].get() == &n) { cycle_start = i; break; }
        }
        for (std::size_t i = cycle_start; i < pulling_stack_.size(); ++i) {
            if (!pulling_stack_[i]) continue;
            path += pulling_stack_[i]->effective_debug_name();
            path += " -> ";
        }
        path += n.effective_debug_name();
        throw CircularDependencyError(
            "reactive::Graph::pull detected re-entrant computation; cycle path: "
            + path);
    }

    // MaybeDirty with all parents settled: cheap version-compare path.
    if (n.state() == NodeState::MaybeDirty) {
        bool any_upstream_changed = false;
        n.for_each_source([&](const Edge& e) {
            if (e.source->state() == NodeState::Computing ||
                e.source->version() != e.observed_version) {
                any_upstream_changed = true;
            }
        });
        if (!any_upstream_changed) {
            n.set_state_(NodeState::Clean);
            return false;
        }
        n.mark_dirty();
    }

    // Dirty: perform the actual recomputation. Keep pulling_stack_
    // consistent across exceptions via a small RAII guard.
    // Keep self-cancelling reactions and their captures alive through all
    // post-callback state writes, exception handling and propagation.
    auto keep_alive = n.retain_for_recompute();
    detail::NodeHandle target{&n};
    pulling_stack_.emplace_back(&n);
    struct StackGuard {
        std::vector<detail::NodeHandle>* stack;
        ~StackGuard() { if (stack) stack->pop_back(); }
    } guard{&pulling_stack_};

    n.set_state_(NodeState::Computing);
    bool changed = false;
    try {
        changed = n.recompute();
    } catch (...) {
        // Leave the node in a recoverable state before unwinding.
        if (target) target->set_state_(NodeState::Clean);
        throw;
    }
    if (!target) return false;
    n.set_state_(NodeState::Clean);

    if (changed) {
        // Value actually moved -> seed the next round with our downstream.
        n.mark_downstream_maybe_dirty();
        n.for_each_observer([&](Edge& e) {
            this->enqueue_dirty(*e.observer);
        });
    }
    return changed;
}

}  // namespace aria::reactive
