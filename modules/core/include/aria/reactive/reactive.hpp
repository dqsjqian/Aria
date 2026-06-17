#pragma once

// ============================================================================
//  reactive/reactive.hpp
// ----------------------------------------------------------------------------
//  Single-include umbrella for the Aria reactive subsystem.
//
//  End users should prefer this header over the individual files: the
//  sub-headers are carefully ordered here so that forward declarations and
//  out-of-line template definitions are all satisfied in one shot.
//
//  Public surface provided by this header:
//    * reactive::Property<T>           -- observable source
//    * reactive::Computed<T>           -- derived value (auto-tracked)
//    * reactive::Effect                -- side-effect reaction (auto-tracked)
//    * reactive::Observer              -- RAII handle returned by subscribe
//    * reactive::batch / BatchScope    -- coalesce multiple writes
//    * reactive::untracked / UntrackedScope -- opt out of tracking
//    * reactive::dep(x)                -- explicit dependency declaration
//    * reactive::CircularDependencyError
//
//  Design rationale: every piece of state that participates in reactivity
//  is a Node in a single process-wide DAG. Writes propagate in two
//  phases (push coloring, pull evaluation) so that computations are
//  evaluated at most once per batch and always after their upstreams,
//  yielding a glitch-free reactive system comparable in guarantees to
//  MobX / SolidJS / Svelte 5's $state + $derived + $effect.
// ============================================================================

// Include order matters -- see the notes above each sub-header. In short:
// graph / node first (they are the protocol), then the three user types,
// with effect.hpp last because it supplies the out-of-line definitions
// of Computed's observer methods.
#include "aria/reactive/node.hpp"
#include "aria/reactive/graph.hpp"
#include "aria/reactive/graph.inl"      // inline implementations of Graph / Node
#include "aria/reactive/property.hpp"
#include "aria/reactive/computed.hpp"
#include "aria/reactive/effect.hpp"
#include "aria/reactive/inspector.hpp"  // diagnostics: to_dot / to_json / flush tracer

// ============================================================================
//  Public-API promotion
// ----------------------------------------------------------------------------
//  The implementation lives in `aria::reactive` (an internal namespace),
//  but every user-visible type is promoted into `aria::` via the using
//  declarations below. Per `docs/api-style.md` S-1/S-2, public code MUST
//  use the unqualified `aria::` form -- the `aria::reactive::` qualified
//  names exist only as an implementation locator (e.g. for users who
//  reach into `Graph::is_on_graph_thread()` for low-level diagnostics).
// ============================================================================
namespace aria {

using reactive::Property;
using reactive::Computed;
using reactive::Effect;
using reactive::BatchScope;
using reactive::UntrackedScope;
using reactive::CircularDependencyError;
using reactive::GraphInspector;

using reactive::batch;
using reactive::untracked;
using reactive::dep;

}  // namespace aria
