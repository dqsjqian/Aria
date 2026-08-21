# ============================================================================
#  Cross-DSO IProperty ABI acceptance test.
# ----------------------------------------------------------------------------
#  Host exe owns the Property<T> objects; the plugin SHARED library
#  manipulates them only through the stable, non-template aria::IProperty
#  interface, across a real shared-library boundary. This validates that
#  std::any / std::type_info / std::function / aria::Subscription all
#  cross a DSO boundary intact, and that a host-owned reactive node can be
#  released from the plugin side.
#
#  This lives under tests/acceptance rather than examples: it is a
#  cross-binary contract gate, not a user-facing plugin demo. It remains
#  intentionally narrow rather than becoming a plugin framework.
#  To extend it into a true
#  libc++-vs-libstdc++ check, build this single directory with a different
#  -stdlib= than the host; the C-linkage entry point keeps the symbol
#  resolvable across that split.
# ============================================================================

# ── Plugin: separate shared library, IProperty-only, no Property<T> here ──
add_library(aria_test_iproperty_plugin SHARED plugin.cpp)
add_library(aria::tests::iproperty_plugin ALIAS aria_test_iproperty_plugin)
target_link_libraries(aria_test_iproperty_plugin PRIVATE aria::core aria::warnings)
target_include_directories(aria_test_iproperty_plugin
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(aria_test_iproperty_plugin PRIVATE ARIA_PLUGIN_BUILD)
set_target_properties(aria_test_iproperty_plugin PROPERTIES
    # Symbols are hidden by default project-wide; the one C entry point is
    # explicitly exported via ARIA_PLUGIN_EXPORT in the header.
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

# ── Host: instantiates the real Property<T>, links the plugin dylib ──
# Links aria::runtime as well as aria::core: the reactive Graph's flush
# path references the process-wide diagnostics-sink storage, which is the
# single shared definition compiled into libaria_runtime (so the exe and
# every DSO share one sink instance).
add_executable(test_cross_dylib_iproperty host.cpp)
target_link_libraries(test_cross_dylib_iproperty
    PRIVATE aria::core aria::runtime aria::warnings aria_test_iproperty_plugin)
target_include_directories(test_cross_dylib_iproperty
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME cross_dylib_abi_smoke COMMAND test_cross_dylib_iproperty)
