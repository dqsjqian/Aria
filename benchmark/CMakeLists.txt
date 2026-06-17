# Benchmark suite (split into focused binaries — see ROADMAP Stage 3.2).
#
# Each binary is independently runnable so CI can select a subset.
# They share `bench_common.hpp` for timing helpers and compile with
# -O3 -DNDEBUG.
#
# `aria_bench_command` is also installed under the legacy name
# `aria_bench` via a symlink-style post-build copy, so existing CI /
# scripts that invoke `aria_bench` keep working without paying to
# compile bench_command.cpp twice.

set(_bench_targets
    aria_bench_command       # Property / Computed / EventBus / Container
    aria_bench_list          # ObservableList mutations
    aria_bench_derived_list  # FilteredList / SortedList / MappedList
    aria_bench_async_command # AsyncCommand / Task / executor
    aria_bench_iproperty     # IProperty type-erased vs template-direct
)

add_executable(aria_bench_command       bench_command.cpp)
add_executable(aria_bench_list          bench_list.cpp)
add_executable(aria_bench_derived_list  bench_derived_list.cpp)
add_executable(aria_bench_async_command bench_async_command.cpp)
add_executable(aria_bench_iproperty     bench_iproperty.cpp)

foreach(_t IN LISTS _bench_targets)
    target_link_libraries(${_t} PRIVATE
        aria::core aria::runtime aria::async aria::warnings
    )
    if(MSVC)
        target_compile_options(${_t} PRIVATE $<$<CONFIG:Release>:/O2>)
    else()
        target_compile_options(${_t} PRIVATE $<$<CONFIG:Release>:-O3>)
    endif()
    target_compile_definitions(${_t} PRIVATE $<$<CONFIG:Release>:NDEBUG>)
    target_include_directories(${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

# Back-compat: expose `aria_bench` as a thin post-build copy of
# aria_bench_command rather than rebuilding bench_command.cpp a second
# time. The copy runs inside the build directory so test / packaging
# scripts that invoke `./benchmark/aria_bench` keep working.
add_custom_target(aria_bench ALL
    DEPENDS aria_bench_command
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:aria_bench_command>
            $<TARGET_FILE_DIR:aria_bench_command>/aria_bench${CMAKE_EXECUTABLE_SUFFIX}
    COMMENT "Creating aria_bench (alias for aria_bench_command)"
)
