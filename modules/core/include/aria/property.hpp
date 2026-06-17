#pragma once

// ============================================================================
//  aria/property.hpp
// ----------------------------------------------------------------------------
//  Convenience entry point that re-exports the reactive-graph `Property<T>`.
//
//  In Aria there is ONE Property implementation -- a Source node in the
//  reactive graph defined under `aria/reactive/property.hpp`. This
//  header is kept at the canonical include path (`<aria/property.hpp>`)
//  so that users never have to learn about the internal reactive/ folder
//  layout; it simply pulls the whole reactive subsystem in.
//
//  If you want direct access to the graph APIs (batch, untracked, Effect,
//  Computed, dep, ...), include `<aria/reactive/reactive.hpp>` instead.
// ============================================================================

#include "aria/reactive/reactive.hpp"
