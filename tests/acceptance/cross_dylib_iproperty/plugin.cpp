// ============================================================================
//  plugin.cpp — the "plugin side" of the cross-dylib ABI smoke.
// ----------------------------------------------------------------------------
//  Built as a SHARED library (libaria_property_plugin). It includes ONLY
//  `aria/i_property.hpp` (+ standard headers) — crucially NOT
//  `aria/reactive/property.hpp`. So this translation unit instantiates no
//  `Property<T>`, owns no reactive Graph, and depends on no Aria template
//  symbol. Everything it does goes through the stable `aria::IProperty`
//  vtable, which is the framework's advertised ABI surface.
//
//  Each numbered return code marks the first failing ABI invariant so the
//  host can report exactly what broke across the DSO boundary.
// ============================================================================
#include "aria_plugin_abi.h"

#include "aria/i_property.hpp"

#include <any>
#include <string>
#include <typeinfo>

extern "C" ARIA_PLUGIN_EXPORT int
aria_plugin_exercise(aria::IProperty* int_prop, aria::IProperty* str_prop) {
    if (int_prop == nullptr || str_prop == nullptr) {
        return 1;
    }

    // ── 1. RTTI across the DSO boundary ──────────────────────────────────
    // `std::type_info::operator==` must compare equal even though the
    // typeid was emitted in the *host* translation unit. This is the
    // classic libstdc++ "RTTI across dlopen" hazard; if it failed, every
    // type-gated dynamic-binding system would silently misroute.
    if (int_prop->type() != typeid(int)) {
        return 2;
    }
    if (str_prop->type() != typeid(std::string)) {
        return 3;
    }

    // ── 2. std::any read across the boundary (SBO payload: int) ──────────
    {
        const std::any boxed = int_prop->get_any();
        const int* unboxed = std::any_cast<int>(&boxed);
        if (unboxed == nullptr) {
            return 4;
        }
    }

    // ── 3. std::any write across the boundary ────────────────────────────
    // Correct payload type → accepted.
    if (!int_prop->set_any(std::any{int{42}})) {
        return 5;
    }
    // Wrong payload type → rejected, property left untouched (IProperty
    // contract: no-throw, returns false on type mismatch).
    if (int_prop->set_any(std::any{std::string{"not an int"}})) {
        return 6;
    }

    // ── 4. subscribe across the boundary; Subscription RAII crosses back ──
    // The callback is a plugin-side std::function stored inside a host-side
    // reactive node. The returned Subscription handle is destroyed *here*
    // (in the plugin), which releases the host-owned node across the DSO
    // boundary — the most fragile lifetime path in the whole interface.
    int callbacks = 0;
    int last_seen = 0;
    {
        aria::Subscription sub = int_prop->subscribe_any(
            [&callbacks, &last_seen](const std::any& value) {
                if (const int* iv = std::any_cast<int>(&value)) {
                    ++callbacks;
                    last_seen = *iv;
                }
            });

        if (!int_prop->set_any(std::any{int{7}})) {
            return 7;
        }
        if (callbacks != 1 || last_seen != 7) {
            return 8;
        }
        // `sub` destructs at the end of this scope.
    }

    // After unsubscribe, further writes must NOT invoke the callback.
    if (!int_prop->set_any(std::any{int{9}})) {
        return 9;
    }
    if (callbacks != 1) {
        return 10;
    }

    // ── 5. heap-spilling std::any payload (std::string) across boundary ──
    if (!str_prop->set_any(std::any{std::string{"hello-from-plugin"}})) {
        return 11;
    }

    return 0;
}
