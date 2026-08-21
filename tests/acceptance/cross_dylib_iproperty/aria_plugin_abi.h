// ============================================================================
//  aria_plugin_abi.h — the stable C-linkage contract between the host
//  executable and the dynamically-loaded plugin in this cross-dylib
//  ABI smoke (ROADMAP P2-A).
// ----------------------------------------------------------------------------
//  The host *owns* the framework objects (`aria::Property<T>`, which is a
//  header-only template instantiated in the host translation unit). The
//  plugin never instantiates a `Property<T>`; it manipulates the host's
//  objects ONLY through the stable, non-template `aria::IProperty`
//  interface. That is the exact shape Aria advertises as ABI-stable:
//  templates stay in the owning module, plug-ins speak the type-erased
//  surface.
//
//  Why the entry point is `extern "C"`:
//    * The *symbol name* is C-mangled, so the host can resolve it whether
//      the plugin was built by a different C++ compiler / standard library.
//    * The *parameter types* are still C++ (`aria::IProperty*`), but only
//      their vtable layout and the std-library payload types they traffic
//      in (`std::any`, `std::type_info`, `std::function`,
//      `aria::Subscription`) cross the boundary — never a mangled
//      `Property<T>` symbol.
// ============================================================================
#pragma once

namespace aria {
class IProperty;  // forward decl only — no template, no header coupling
}

#if defined(_WIN32)
#  if defined(ARIA_PLUGIN_BUILD)
#    define ARIA_PLUGIN_EXPORT __declspec(dllexport)
#  else
#    define ARIA_PLUGIN_EXPORT __declspec(dllimport)
#  endif
#else
#  define ARIA_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

extern "C" {

/// Exercise the host-owned properties purely through `aria::IProperty`.
///
/// Returns 0 on success; a small non-zero code identifies the first
/// failing ABI assertion (see plugin.cpp). The host turns a non-zero
/// return into a failed CTest run.
///
/// Contract: invoked on the host's graph thread (the main thread here),
/// so every `IProperty` virtual is called on the correct thread per the
/// lifecycle doc (L-1).
ARIA_PLUGIN_EXPORT int aria_plugin_exercise(aria::IProperty* int_prop,
                                            aria::IProperty* str_prop);

}  // extern "C"
