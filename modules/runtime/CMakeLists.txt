if(ARIA_BUILD_SHARED)
    add_library(aria_runtime SHARED)
else()
    add_library(aria_runtime STATIC)
endif()
add_library(aria::runtime ALIAS aria_runtime)

target_sources(aria_runtime PRIVATE
    src/logger.cpp
    src/container.cpp
    src/event_bus.cpp
    src/dispatcher.cpp
    src/diagnostics_sink.cpp
    src/runtime_bootstrap.cpp
)

target_include_directories(aria_runtime PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(aria_runtime PUBLIC cxx_std_20)

# aria::async is a header-only INTERFACE target that only depends on
# aria::core, so linking it here creates no cycle. Needed by
# include/aria/runtime/dispatcher_executor.hpp, which bridges
# IDispatcher to async::IExecutor.
target_link_libraries(aria_runtime
    PUBLIC aria::core aria::abi aria::async Threads::Threads
    PRIVATE aria::warnings
)

target_compile_definitions(aria_runtime PRIVATE ARIA_RUNTIME_BUILD)
if(NOT ARIA_BUILD_SHARED)
    target_compile_definitions(aria_runtime PUBLIC ARIA_RUNTIME_STATIC)
endif()

set_target_properties(aria_runtime PROPERTIES
    SOVERSION ${PROJECT_VERSION_MAJOR}
    VERSION ${PROJECT_VERSION}
)

if(ARIA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
