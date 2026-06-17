#pragma once

// ============================================================================
//  reactive/node.hpp
// ----------------------------------------------------------------------------
//  Defines `Node` -- the core abstraction of the reactive graph.
//
//  In Aria, every entity that participates in dependency tracking
//  (Property, AutoComputed, Effect) embeds or inherits a `Node` and is
//  registered in the single process-wide DAG.
//
//  Design highlights
//  -----------------
//  1. Two-phase Push-Pull propagation. When a source changes, we only
//     recursively "color" (mark MaybeDirty) the downstream; actual
//     recomputation is deferred to batch-end / explicit flush. This is the
//     key ingredient that eliminates glitches.
//  2. Monotonic `version` per node. When a downstream node pulls from an
//     upstream, it remembers the upstream's version. On the next pull it
//     compares again and skips recomputation if nothing actually changed.
//  3. Intrusive doubly-linked edge lists. Each Edge is threaded into both
//     the source's "observers" list and the observer's "sources" list,
//     giving O(1) subscribe / unsubscribe with no heap allocation (the
//     Edge itself lives inside the observer or a dedicated pool).
//  4. Single-thread assumption. All graph mutations must happen on the UI
//     thread. Debug builds assert on thread identity at public entry
//     points. Cross-thread updates must be marshalled via a Dispatcher.
// ============================================================================

#include <atomic>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace aria::reactive {

class Node;      // forward
class Graph;     // forward
struct Edge;

// ---------------------------------------------------------------------------
//  Node role:
//    - Source     : an actively mutable root node (Property is a Source)
//    - Derivation : value is computed from upstreams via compute()
//                   (AutoComputed is a Derivation)
//    - Reaction   : pure side-effect node (Effect / bind callback). Its
//                   value is not exposed; it only runs an action on change.
// ---------------------------------------------------------------------------
enum class NodeKind : std::uint8_t {
    Source,
    Derivation,
    Reaction,
};

// ---------------------------------------------------------------------------
//  Node state during a flush cycle:
//    - Clean      : cached value matches all upstreams; pull is a no-op.
//    - MaybeDirty : an ancestor may have changed; must verify upstream
//                   versions on pull before deciding whether to recompute.
//    - Dirty      : at least one upstream has definitely changed; must
//                   recompute.
//    - Computing  : currently running compute(); used to detect cycles.
// ---------------------------------------------------------------------------
enum class NodeState : std::uint8_t {
    Clean      = 0,
    MaybeDirty = 1,
    Dirty      = 2,
    Computing  = 3,
};

/// A single dependency edge: (upstream source) -> (downstream observer).
/// Threaded into two intrusive linked lists so subscribe/unsubscribe are O(1)
/// without heap allocation. Edges are owned by the observer (embedded inline
/// in the Derivation's source-array or allocated from a dedicated pool).
struct Edge {
    Node* source   = nullptr;   ///< The upstream node being observed.
    Node* observer = nullptr;   ///< The downstream node that depends on it.

    /// Thread in the source's "observers" list (from the source's point of
    /// view: "these are the nodes watching me").
    Edge* next_observer = nullptr;
    Edge* prev_observer = nullptr;

    /// Thread in the observer's "sources" list (from the observer's point
    /// of view: "these are the nodes I depend on").
    Edge* next_source = nullptr;
    Edge* prev_source = nullptr;

    /// Upstream version observed at the moment this edge was established or
    /// last confirmed. On pull, a mismatch with `source->version()` means
    /// the upstream has moved and the observer must recompute.
    std::uint64_t observed_version = 0;
};

/// Common base for every node participating in the reactive graph.
/// Concrete nodes implement `recompute()` (Derivation / Reaction) or drive
/// `notify_changed()` externally after mutating their value (Source).
class Node {
    // Graph is the single orchestrator of push-color / pull-evaluate and
    // therefore needs privileged access to Node's intrusive lists and
    // lifecycle state. All other code must go through the public surface.
    friend class Graph;

public:
    explicit Node(NodeKind kind) noexcept : kind_(kind) {}
    virtual ~Node() noexcept;

    Node(const Node&)            = delete;
    Node& operator=(const Node&) = delete;
    Node(Node&&)                 = delete;
    Node& operator=(Node&&)      = delete;

    // ---- Read-only accessors ---------------------------------------------
    [[nodiscard]] NodeKind      kind()    const noexcept { return kind_; }
    [[nodiscard]] NodeState     state()   const noexcept { return state_; }
    [[nodiscard]] std::uint64_t version() const noexcept { return version_; }
    [[nodiscard]] const std::string& debug_name() const noexcept { return debug_name_; }

    void set_debug_name(std::string name) { debug_name_ = std::move(name); }

    /// A non-empty debug label for diagnostic output.
    ///
    /// If the user explicitly set a `debug_name()`, that is returned
    /// verbatim. Otherwise a deterministic per-node fallback of the
    /// form `"<Kind>#<id>"` (e.g. `"Derivation#42"`) is lazily
    /// cached and returned — so stderr dumps and GraphInspector
    /// labels are always meaningful even when the host type forgot
    /// to name its nodes.
    ///
    /// The fallback id is a process-wide monotonic counter, not the
    /// node's address, so diagnostic output stays stable across
    /// runs and ASLR shuffles.
    [[nodiscard]] const std::string& effective_debug_name() const {
        if (!debug_name_.empty()) return debug_name_;
        if (fallback_name_.empty()) {
            const char* kind = nullptr;
            switch (kind_) {
                case NodeKind::Source:     kind = "Source";     break;
                case NodeKind::Derivation: kind = "Derivation"; break;
                case NodeKind::Reaction:   kind = "Reaction";   break;
            }
            fallback_name_ = std::string(kind ? kind : "Node")
                             + "#" + std::to_string(node_id_);
        }
        return fallback_name_;
    }

    /// Returns the process-wide singleton Graph this node belongs to.
    static Graph& graph() noexcept;

    // ---- Methods below are intended for Graph / derived classes.          -
    // ---- They are public to simplify the implementation, not as API.      -

    /// Called by a Source after its value has actually changed: bumps the
    /// version and colors all downstream nodes MaybeDirty. If no batch is
    /// active, the Graph will immediately flush afterwards.
    void notify_changed();

    /// Mark self and all reachable descendants as MaybeDirty. Nodes already
    /// in a non-Clean state are skipped, bounding the walk to O(|affected|).
    void mark_downstream_maybe_dirty();

    /// Escalate state to Dirty (used by Graph::pull after confirming an
    /// upstream has truly moved).
    void mark_dirty() noexcept { state_ = NodeState::Dirty; }

    /// Topological depth used by flush ordering. Source nodes are depth 0;
    /// Derivations are (max upstream depth + 1). Updated on edge insertion.
    [[nodiscard]] std::uint32_t depth() const noexcept { return depth_; }
    void                         set_depth(std::uint32_t d) noexcept { depth_ = d; }

    /// Recompute hook for Derivation / Reaction nodes. Source nodes keep
    /// the default empty implementation (never invoked).
    /// Return true iff the cached value actually changed (version already
    /// bumped inside the override); this tells the Graph to propagate to
    /// downstream nodes.
    virtual bool recompute() { return false; }

    // ---- Edge manipulation (intrusive linked list) -----------------------
    void attach_as_observer_of(Node& source, Edge& edge);
    void detach_edge(Edge& edge) noexcept;

    /// Drop every upstream edge. Typical use: a Derivation calls this just
    /// before recomputing so it can gather a fresh dependency set.
    void clear_sources() noexcept;

    /// Iterate the observer list (used by Graph to color downstream nodes).
    template<class F>
    void for_each_observer(F&& f) {
        for (Edge* e = observers_head_; e != nullptr; e = e->next_observer) {
            f(*e);
        }
    }

    /// Iterate the source list (used by a Derivation in pull() to compare
    /// each upstream's current version against `observed_version`).
    template<class F>
    void for_each_source(F&& f) const {
        for (const Edge* e = sources_head_; e != nullptr; e = e->next_source) {
            f(*e);
        }
    }

    [[nodiscard]] bool has_observers() const noexcept { return observers_head_ != nullptr; }
    [[nodiscard]] bool has_sources()   const noexcept { return sources_head_   != nullptr; }

protected:
    // Derived classes call this when a recompute actually changes the value.
    void bump_version_() noexcept { ++version_; }
    void set_state_(NodeState s) noexcept { state_ = s; }

private:
    NodeKind      kind_;
    NodeState     state_      = NodeState::Clean;
    std::uint32_t depth_      = 0;
    std::uint64_t version_    = 1;   ///< Starts at 1; 0 means "never observed".

    // Intrusive list heads. Could be made circular to drop the prev
    // pointers; kept doubly-linked for clarity of implementation.
    Edge* observers_head_ = nullptr;   ///< My downstream edges.
    Edge* sources_head_   = nullptr;   ///< My upstream edges.

    std::string         debug_name_;         ///< User-supplied diagnostic label.
    mutable std::string fallback_name_;      ///< Lazy `"<Kind>#<id>"` cache.

    /// Process-wide monotonic node id assigned at construction. Drives
    /// the fallback debug label so output is stable across runs.
    std::uint64_t node_id_ = next_node_id_();

    static std::uint64_t next_node_id_() noexcept {
        static std::atomic<std::uint64_t> counter{0};
        return ++counter;
    }
};

}  // namespace aria::reactive

namespace aria {

/// Type that participates in the reactive graph: inherits from
/// `aria::reactive::Node`. Used by `aria::dep(x)` (and any future
/// helper that wants to accept "any reactive cell") to surface a
/// one-line diagnostic instead of a SFINAE explosion.
///
/// Surfaced in `aria::` per `docs/api-style.md` S-1: users never have
/// to qualify a constraint name with the implementation namespace.
template<typename T>
concept ReactiveNode = std::derived_from<T, ::aria::reactive::Node>;

}  // namespace aria
