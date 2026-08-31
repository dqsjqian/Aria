add_executable(aria_fuzz
    fuzz_main.cpp
    fuzz_signal_unsubscribe_during_emit.cpp
    fuzz_computed_dynamic_dep.cpp
    fuzz_reactive_reentrant_set.cpp
    fuzz_binding_view_destroy_race.cpp
    fuzz_observable_list_mutation_storm.cpp
    fuzz_cancellation_race.cpp
    fuzz_async_command_dtor.cpp
    fuzz_when_any_race.cpp
    fuzz_coroutine_scope_drain.cpp
    # Diagnostic-protocol invariants (docs/reference/diagnostics.md §7).
    fuzz_trace_sink_install_race.cpp
    fuzz_trace_sink_throw_swallow.cpp
    fuzz_trace_sink_scoped_nesting.cpp
    # Error-model invariants (docs/reference/error-model.md §7).
    fuzz_error_from_exception_table.cpp
    fuzz_error_property_equality_gate.cpp
    fuzz_validator_field_path.cpp
    fuzz_async_command_cancellation_no_error.cpp
)

target_include_directories(aria_fuzz PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(aria_fuzz PRIVATE
    aria::core
    aria::async
    aria::binding
    # The global trace-sink slot lives in aria_runtime
    # (runtime/src/diagnostics_sink.cpp), which the trace-sink fuzzers
    # drive directly.
    aria::runtime
    doctest::doctest
    aria::warnings
)

# Fuzzers run with the default modest iteration count (50k each), keeping
# total runtime well under one minute on a developer machine. Set
# `ARIA_FUZZ_ITERS=1000000` (and optionally `ARIA_FUZZ_SEED=...`) for
# nightly / pre-release runs that target the lifecycle.md headline
# numbers (1M iterations / fuzzer).
add_test(NAME fuzz_tests COMMAND aria_fuzz)
