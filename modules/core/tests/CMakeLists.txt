add_executable(test_core
    test_main.cpp
    test_property.cpp
    test_computed.cpp
    test_command.cpp
    test_observable_list.cpp
        test_filtered_list.cpp
    test_derived_chaining.cpp
    test_sorted_list.cpp
    test_mapped_list.cpp
    test_derived_chain.cpp
    test_validator.cpp
    test_property_ops.cpp
    test_selection.cpp
    test_todomvc_workflow.cpp
    test_inspector.cpp
    test_i_property.cpp
    test_lifecycle_invariants.cpp
    test_error_model.cpp
    test_list_conformance.cpp
    test_list_signal_mixin.cpp
    test_diagnostics.cpp
    test_loadable.cpp
    test_distinct_list.cpp
    test_paged_list.cpp
    test_grouped_list.cpp
    test_scheduler.cpp
    test_function_ref.cpp
    test_inplace_function.cpp
    test_callback_boundary.cpp
)
target_link_libraries(test_core PRIVATE
    aria::core
    aria::async
    aria::runtime
    doctest::doctest
    aria::warnings
)
add_test(NAME core_tests COMMAND test_core)

# ── JNI RecyclerView list bridge (host-testable half) ───────────────────
#
# `aria/adapters/jni/JniListSource.hpp` owns the snapshot and the
# ListChange -> notifyItem* diffing and has no <jni.h> dependency, which
# is why its test is registered here rather than under the ANDROID-gated
# `modules/adapters/jni/tests/`: the rest of that adapter can only be
# static_asserted on a host, and a list bridge whose row arithmetic is
# never executed is how off-by-one row bugs reach a device.
#
# Only the JNI half (`JniRecyclerNotifier`, which drives notifyItem*
# through a real VM) stays device-bound. This target links aria::core
# alone — linking aria::adapters::jni would drag in <jni.h> and defeat
# the split.
add_executable(test_jni_list_source
    ${CMAKE_SOURCE_DIR}/modules/adapters/jni/tests/test_jni_list_source.cpp
)
target_include_directories(test_jni_list_source PRIVATE
    ${CMAKE_SOURCE_DIR}/modules/adapters/jni/include
)
target_link_libraries(test_jni_list_source PRIVATE
    aria::core
    # ObservableList publishes trace::List events, and the global sink
    # storage those go through lives in aria_runtime (see
    # runtime/src/diagnostics_sink.cpp) — needed even though this test
    # installs no sink.
    aria::runtime
    doctest::doctest
    aria::warnings
)
add_test(NAME jni_list_source COMMAND test_jni_list_source)
