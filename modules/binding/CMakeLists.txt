if(ARIA_BUILD_SHARED)
    add_library(aria_binding SHARED)
else()
    add_library(aria_binding STATIC)
endif()
add_library(aria::binding ALIAS aria_binding)

target_sources(aria_binding PRIVATE
    src/binding.cpp
    src/view_adapter_base.cpp
    src/view_model.cpp
)

target_include_directories(aria_binding PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(aria_binding PUBLIC cxx_std_20)

target_link_libraries(aria_binding
    PUBLIC aria::core aria::async aria::runtime
    PRIVATE aria::warnings
)

target_compile_definitions(aria_binding PRIVATE ARIA_BINDING_BUILD)
if(NOT ARIA_BUILD_SHARED)
    target_compile_definitions(aria_binding PUBLIC ARIA_BINDING_STATIC)
endif()

set_target_properties(aria_binding PROPERTIES
    SOVERSION ${PROJECT_VERSION_MAJOR}
    VERSION ${PROJECT_VERSION}
)

if(ARIA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
