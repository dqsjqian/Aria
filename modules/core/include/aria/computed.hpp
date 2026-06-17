#pragma once

// ============================================================================
//  aria/computed.hpp
// ----------------------------------------------------------------------------
//  Convenience entry point that re-exports the reactive-graph `Computed<T>`.
//
//  There is ONE Computed implementation, defined in
//  `aria/reactive/computed.hpp`. Its dependency set is discovered
//  automatically at every recompute via auto-tracking -- you no longer
//  have to list upstream sources at construction.
//
//  Equivalent to `#include <aria/reactive/reactive.hpp>`.
// ============================================================================

#include "reactive/reactive.hpp"
