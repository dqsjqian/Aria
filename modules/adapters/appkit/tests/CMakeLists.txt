# AppKit conformance tests — drive the shared adapter_conformance
# battery against the production AppKitAdapter.

if(NOT APPLE)
    return()
endif()

add_executable(test_appkit_conformance
    test_appkit_conformance.mm
)

target_link_libraries(test_appkit_conformance PRIVATE
    aria::adapters::appkit
    aria::core
    aria::binding
    aria::async
    doctest::doctest
)

# ARC for the .mm
target_compile_options(test_appkit_conformance PRIVATE -fobjc-arc)

# ARIA_BUILD_TESTS already pulled doctest above.
add_test(NAME appkit_conformance COMMAND test_appkit_conformance)

# ── List-source bridge tests ─────────────────────────────────────────────
# `ObservableTableSource<T>` is a header-only template, so this binary
# is a separate doctest entry point that drives it against an off-screen
# NSTableView. Pin the bridge contract for ObservableList / FilteredList
# / MappedList sources, including the `Move` event.

add_executable(test_appkit_table_source
    test_appkit_table_source.mm
)

target_link_libraries(test_appkit_table_source PRIVATE
    aria::adapters::appkit
    aria::core
    aria::binding
    aria::async
    doctest::doctest
)

target_compile_options(test_appkit_table_source PRIVATE -fobjc-arc)

add_test(NAME appkit_table_source COMMAND test_appkit_table_source)

# ── view_for / release_view ──────────────────────────────────────────────
# Handle → IView ownership moved from every host's local keepalive vector
# into the adapter. Separate doctest entry point (own main).

add_executable(test_appkit_view_for
    test_appkit_view_for.mm
)

target_link_libraries(test_appkit_view_for PRIVATE
    aria::adapters::appkit
    aria::core
    aria::binding
    aria::async
    doctest::doctest
)

target_compile_options(test_appkit_view_for PRIVATE -fobjc-arc)

add_test(NAME appkit_view_for COMMAND test_appkit_view_for)
