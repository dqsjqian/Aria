add_executable(test_uikit_conformance
    test_uikit_conformance.mm
)

target_link_libraries(test_uikit_conformance PRIVATE
    aria::adapters::uikit
    aria::core
    aria::binding
    aria::async
    doctest::doctest
)

target_compile_options(test_uikit_conformance PRIVATE -fobjc-arc)

add_test(NAME uikit_conformance COMMAND test_uikit_conformance)

# ── UIKitTableSource list bridge ─────────────────────────────────────────
#
# Separate binary rather than more cases in test_uikit_conformance: that
# target drives the shared IViewAdapter conformance battery, while this
# one exercises the list/table bridge. Keeping them apart means a table
# regression names itself in CI instead of hiding inside "conformance".
#
# Mirrors `appkit/tests/test_appkit_table_source.mm`. Runs in the iOS
# simulator via the same `uikit` CI job — see .github/workflows/ci.yml,
# which builds and runs both simulator targets.
add_executable(test_uikit_table_source
    test_uikit_table_source.mm
)

target_link_libraries(test_uikit_table_source PRIVATE
    aria::adapters::uikit
    aria::core
    aria::binding
    doctest::doctest
)

target_compile_options(test_uikit_table_source PRIVATE -fobjc-arc)

add_test(NAME uikit_table_source COMMAND test_uikit_table_source)
