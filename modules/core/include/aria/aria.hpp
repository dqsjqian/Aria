#pragma once

// aria — single-include header for the core library.
// Pull in async/runtime/binding separately as needed.
//
// Version macros / ABI plumbing live under `aria/abi/version.hpp` and
// should be included explicitly by the libraries that need them
// (shared-library version-export helpers, etc.), not by ordinary
// user code that only wants the reactive primitives.

#include "aria/concepts.hpp"
#include "aria/callback_boundary.hpp"
#include "aria/function_ref.hpp"
#include "aria/inplace_function.hpp"
#include "aria/scheduler.hpp"
#include "aria/subscription.hpp"
#include "aria/validation_key.hpp"
#include "aria/error.hpp"
#include "aria/loadable.hpp"
#include "aria/diagnostics.hpp"
#include "aria/property.hpp"
#include "aria/computed.hpp"
#include "aria/command.hpp"
#include "aria/observable_list.hpp"
#include "aria/list_source.hpp"
#include "aria/validator.hpp"
