add_library(aria_app INTERFACE)
add_library(aria::app ALIAS aria_app)
target_include_directories(aria_app INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(aria_app INTERFACE cxx_std_23)
target_link_libraries(aria_app INTERFACE aria::binding aria::runtime)
set_target_properties(aria_app PROPERTIES EXPORT_NAME app)
if(ARIA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
