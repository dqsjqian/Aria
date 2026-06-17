#pragma once

// ============================================================================
//  reactive/inspector.hpp
// ----------------------------------------------------------------------------
//  `GraphInspector` is a diagnostics-only facade over the reactive graph.
//
//  The reactive engine is intentionally opaque at runtime: nodes are owned
//  by their Property / Computed / Effect host objects, edges are intrusive,
//  and the Graph does not keep a global registry (both for performance and
//  to avoid lifetime pitfalls). That opacity becomes painful the first
//  time a user asks "why did Computed<X> not recompute?" or "why did the
//  Effect fire twice after a single set()?"
//
//  Inspector addresses this in three small, independently useful pieces:
//
//    1. **Structural snapshot** -- `to_dot()` / `to_json()` walk the DAG
//       from a set of seed nodes (typically the VM's Properties and
//       Computeds) and emit a textual dump of the reachable subgraph.
//       Open the .dot in Graphviz or pipe the JSON into any tool.
//
//    2. **Flush tracing** -- `install_flush_tracer()` registers a callback
//       the Graph invokes for every pull during a flush. Combined with
//       `Node::debug_name()`, this gives a complete, ordered record of
//       which nodes re-evaluated on each write, and in what round.
//
//    3. **Cycle path** -- on CircularDependencyError the Graph now
//       captures the pending chain so callers can log the exact node
//       names that formed the cycle.
//
//  The inspector is header-only and opt-in: if you never call it, the
//  graph pays no cost. The flush tracer check is a single pointer
//  comparison on the hot path.
// ============================================================================

#include "aria/reactive/graph.hpp"
#include "aria/reactive/node.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace aria::reactive {

/// Lightweight diagnostics entry point. All methods are static; the
/// class is just a namespace with the benefit of being friend-able.
class GraphInspector {
public:
    // ------------------------------------------------------------------
    //  Structural snapshot
    // ------------------------------------------------------------------

    /// Collect every node reachable from `seeds` through either direction
    /// of the dependency graph (upstream sources AND downstream observers).
    /// Used internally by `to_dot`/`to_json`; exposed for callers that
    /// want to build their own formatter.
    static std::vector<const Node*> reachable_from(
        const std::vector<const Node*>& seeds) {
        std::vector<const Node*> out;
        std::unordered_set<const Node*> visited;
        std::vector<const Node*> stack(seeds.begin(), seeds.end());

        while (!stack.empty()) {
            const Node* n = stack.back();
            stack.pop_back();
            if (!n || !visited.insert(n).second) continue;
            out.push_back(n);

            // Walk sources (upstream).
            n->for_each_source([&](const Edge& e) {
                if (e.source) stack.push_back(e.source);
            });
            // Walk observers (downstream). `for_each_observer` is a
            // non-const member because it exposes mutable Edge& to
            // `Graph` (which needs to touch observed_version); for a
            // const-correct walk we go through the head pointer via a
            // helper.
            walk_observers_(n, [&](const Node* obs) {
                if (obs) stack.push_back(obs);
            });
        }
        return out;
    }

    /// Emit the reachable subgraph as a Graphviz DOT document.
    /// Usage:
    ///   std::ofstream("g.dot") << GraphInspector::to_dot({&prop, &comp});
    ///   dot -Tsvg g.dot -o g.svg
    static std::string to_dot(const std::vector<const Node*>& seeds,
                              std::string_view graph_name = "reactive") {
        std::ostringstream os;
        os << "digraph " << graph_name << " {\n";
        os << "  rankdir=LR;\n";
        os << "  node [shape=box, style=rounded, fontname=\"monospace\"];\n";

        const auto nodes = reachable_from(seeds);

        // One node per reachable Node, labelled with kind/state/depth/name.
        for (const Node* n : nodes) {
            os << "  \"" << reinterpret_cast<std::uintptr_t>(n) << "\" "
               << "[label=\"" << escape_(label_for_(n))
               << "\", " << style_for_(n) << "];\n";
        }

        // One edge per Edge record. We enumerate via each node's source
        // list to avoid double-counting (every edge shows up once as a
        // source of its observer and once as an observer of its source).
        for (const Node* n : nodes) {
            n->for_each_source([&](const Edge& e) {
                if (!e.source || !e.observer) return;
                os << "  \"" << reinterpret_cast<std::uintptr_t>(e.source)
                   << "\" -> \"" << reinterpret_cast<std::uintptr_t>(e.observer)
                   << "\" [label=\"v=" << e.observed_version << "\"];\n";
            });
        }

        os << "}\n";
        return os.str();
    }

    /// Emit the reachable subgraph as a minimal JSON document.
    /// Schema:
    ///   { "nodes": [{"id","kind","state","depth","version","name"}...],
    ///     "edges": [{"from","to","observed_version"}...] }
    static std::string to_json(const std::vector<const Node*>& seeds) {
        std::ostringstream os;
        const auto nodes = reachable_from(seeds);

        os << "{\"nodes\":[";
        bool first = true;
        for (const Node* n : nodes) {
            if (!first) os << ',';
            first = false;
            os << "{\"id\":" << reinterpret_cast<std::uintptr_t>(n)
               << ",\"kind\":\"" << kind_name_(n->kind())
               << "\",\"state\":\"" << state_name_(n->state())
               << "\",\"depth\":" << n->depth()
               << ",\"version\":" << n->version()
               << ",\"name\":\"" << escape_json_(n->debug_name()) << "\"}";
        }
        os << "],\"edges\":[";

        first = true;
        for (const Node* n : nodes) {
            n->for_each_source([&](const Edge& e) {
                if (!e.source || !e.observer) return;
                if (!first) os << ',';
                first = false;
                os << "{\"from\":" << reinterpret_cast<std::uintptr_t>(e.source)
                   << ",\"to\":" << reinterpret_cast<std::uintptr_t>(e.observer)
                   << ",\"observed_version\":" << e.observed_version << '}';
            });
        }
        os << "]}";
        return os.str();
    }

    // ------------------------------------------------------------------
    //  Flush tracing
    // ------------------------------------------------------------------

    /// A single event emitted by the Graph during flush. Phase describes
    /// *when* the event fires; fields are populated per the phase.
    struct FlushEvent {
        enum class Phase {
            FlushBegin,     ///< entering Graph::flush
            RoundBegin,     ///< starting a topological round
            Pull,           ///< about to pull `node`
            SkipClean,      ///< `node` became Clean via an earlier pull
            Recomputed,     ///< pull ran; `changed` = whether value moved
            RoundEnd,       ///< round completed
            FlushEnd,       ///< Graph::flush returning cleanly
        };
        Phase       phase;
        const Node* node        = nullptr;  ///< null for FlushBegin / FlushEnd / Round boundaries
        int         round       = 0;        ///< 1-based round index
        bool        changed     = false;    ///< valid for Phase::Recomputed
        /// Elapsed wall-clock microseconds between the matching
        /// `Pull` and this `Recomputed` event. Zero for all other
        /// phases. Useful for "which nodes are expensive to pull?"
        /// and for feeding a real-time perf overlay.
        long long   duration_us = 0;
    };

    using FlushTracer = std::function<void(const FlushEvent&)>;

    /// Install a tracer. Replaces any previously installed one. Pass an
    /// empty function (or call `clear_flush_tracer()`) to disable.
    ///
    /// Cost when NOT installed: a single pointer load + null check in
    /// Graph::flush / Graph::pull. Cost when installed: one virtual call
    /// (std::function invocation) per event plus a `steady_clock::now()`
    /// pair around every Pull.
    static void install_flush_tracer(FlushTracer tracer) {
        if (!tracer) {
            clear_flush_tracer();
            return;
        }
        // We adapt the user-facing `FlushTracer` (nice FlushEvent struct)
        // onto the low-level `FlushTraceFn` the Graph dispatches to. The
        // phase integer → enum mapping is the stable ABI boundary.
        //
        // `last_pull` is captured mutably inside the adapter: each
        // Pull records its timestamp and the next Recomputed subtracts
        // from it. Graph::flush is single-threaded (the graph-thread
        // invariant) so a plain by-value mutable capture is enough —
        // no heap allocation needed.
        flush_trace_hook_() = [tracer = std::move(tracer),
                               last_pull = std::chrono::steady_clock::time_point{}](
                                  int phase_int,
                                  const Node* node,
                                  int round,
                                  bool changed) mutable {
            FlushEvent ev;
            ev.phase   = static_cast<FlushEvent::Phase>(phase_int);
            ev.node    = node;
            ev.round   = round;
            ev.changed = changed;
            if (ev.phase == FlushEvent::Phase::Pull) {
                last_pull = std::chrono::steady_clock::now();
            } else if (ev.phase == FlushEvent::Phase::Recomputed) {
                const auto now = std::chrono::steady_clock::now();
                ev.duration_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    now - last_pull).count();
            }
            tracer(ev);
        };
    }

    static void clear_flush_tracer() noexcept {
        flush_trace_hook_() = {};
    }

    [[nodiscard]] static bool has_flush_tracer() noexcept {
        return static_cast<bool>(flush_trace_hook_());
    }

    /// RAII guard — install a tracer for the lifetime of a scope, then
    /// restore the previous tracer (or no tracer) on destruction.
    /// Ideal inside tests or short debugging sessions so you cannot
    /// forget to unplug the hook.
    class ScopedTracer {
    public:
        explicit ScopedTracer(FlushTracer tracer) {
            previous_ = std::move(flush_trace_hook_());
            install_flush_tracer(std::move(tracer));
        }
        ~ScopedTracer() { flush_trace_hook_() = std::move(previous_); }
        ScopedTracer(const ScopedTracer&)            = delete;
        ScopedTracer& operator=(const ScopedTracer&) = delete;
    private:
        FlushTraceFn previous_;
    };

    // ------------------------------------------------------------------
    //  Tiny convenience: ASCII dump for quick stderr debugging.
    // ------------------------------------------------------------------

    /// Human-readable one-line-per-node summary, useful inside gdb or
    /// during ad-hoc printf debugging.
    static std::string to_text(const std::vector<const Node*>& seeds) {
        std::ostringstream os;
        for (const Node* n : reachable_from(seeds)) {
            os << "[" << kind_name_(n->kind()) << "]"
               << " " << n->effective_debug_name()
               << "  depth=" << n->depth()
               << "  v=" << n->version()
               << "  state=" << state_name_(n->state()) << '\n';
        }
        return os.str();
    }

private:
    static const char* kind_name_(NodeKind k) noexcept {
        switch (k) {
            case NodeKind::Source:     return "Source";
            case NodeKind::Derivation: return "Derivation";
            case NodeKind::Reaction:   return "Reaction";
        }
        return "?";
    }

    static const char* state_name_(NodeState s) noexcept {
        switch (s) {
            case NodeState::Clean:      return "Clean";
            case NodeState::MaybeDirty: return "MaybeDirty";
            case NodeState::Dirty:      return "Dirty";
            case NodeState::Computing:  return "Computing";
        }
        return "?";
    }

    static std::string label_for_(const Node* n) {
        std::ostringstream os;
        os << kind_name_(n->kind()) << "\\n";
        os << n->effective_debug_name() << "\\n";
        os << "d=" << n->depth()
           << " v=" << n->version()
           << " " << state_name_(n->state());
        return os.str();
    }

    static const char* style_for_(const Node* n) noexcept {
        switch (n->kind()) {
            case NodeKind::Source:     return "fillcolor=\"#cde4ff\", style=\"rounded,filled\"";
            case NodeKind::Derivation: return "fillcolor=\"#ffe3b0\", style=\"rounded,filled\"";
            case NodeKind::Reaction:   return "fillcolor=\"#d8ffd8\", style=\"rounded,filled\"";
        }
        return "";
    }

    static std::string escape_(std::string_view s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
            else if (c == '\n')        { out += "\\n"; }
            else                        { out.push_back(c); }
        }
        return out;
    }

    static std::string escape_json_(std::string_view s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:   out.push_back(c);
            }
        }
        return out;
    }

    // Walk a node's observers via its intrusive list. `Node::for_each_observer`
    // passes `Edge&` (non-const) because Graph needs that mutability; the
    // inspector only needs the downstream Node*, so we read the head
    // directly. This is safe: we never mutate, and the list walk is
    // entirely sequential.
    template<class F>
    static void walk_observers_(const Node* n, F&& f) {
        // const_cast is acceptable here: we only read the list; no Edge
        // field is modified. The signature is non-const purely for
        // mutability in the non-diagnostic callers.
        const_cast<Node*>(n)->for_each_observer([&](const Edge& e) {
            f(e.observer);
        });
    }
};

}  // namespace aria::reactive
