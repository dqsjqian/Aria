// ============================================================================
//  modules/abi/src/version.cpp
// ----------------------------------------------------------------------------
//  Compiled-in counterparts to the compile-time constants in version.hpp.
//
//  These exist so a consumer can detect the one thing header constants
//  structurally cannot: that the library it LOADED is not the one its headers
//  came from. `ARIA_ABI_VERSION` had no reader anywhere in the project before
//  this, which meant the ABI-compatibility promise in docs/architecture.md
//  was unenforceable by construction.
// ============================================================================

#include "aria/abi/version.hpp"

namespace aria::abi {

int runtime_abi_version() noexcept {
    // Deliberately reads the macro, not `abi_version`, so this stays correct
    // if the constant is ever renamed or re-scoped.
    return ARIA_ABI_VERSION;
}

const char* runtime_version_string() noexcept {
    return ARIA_VERSION_STRING;
}

}  // namespace aria::abi
