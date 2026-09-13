if(ARIA_BUILD_SHARED)
    add_library(aria_abi SHARED)
else()
    add_library(aria_abi STATIC)
endif()

target_sources(aria_abi PRIVATE
    src/signal.cpp
    src/slot.cpp
    src/callback_boundary.cpp
    src/diagnostics.cpp
    src/scheduler.cpp
    src/reactive_graph.cpp
    src/version.cpp
)
add_library(aria::abi ALIAS aria_abi)

target_include_directories(aria_abi PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(aria_abi PUBLIC cxx_std_20)

target_compile_definitions(aria_abi PRIVATE ARIA_ABI_BUILD)
if(NOT ARIA_BUILD_SHARED)
    target_compile_definitions(aria_abi PUBLIC ARIA_ABI_STATIC)
endif()

# The compiled foundations implement storage declared by the core headers.
# This is a private include dependency, not a link back to aria::core.
target_include_directories(aria_abi PRIVATE
    "${PROJECT_SOURCE_DIR}/modules/core/include"
)
target_link_libraries(aria_abi PUBLIC Threads::Threads PRIVATE aria::warnings)
set_target_properties(aria_abi PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    SOVERSION ${PROJECT_VERSION_MAJOR}
    VERSION ${PROJECT_VERSION}
)

if(ARIA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
