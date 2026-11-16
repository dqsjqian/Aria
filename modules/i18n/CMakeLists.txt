if(ARIA_BUILD_SHARED)
    add_library(aria_i18n SHARED)
else()
    add_library(aria_i18n STATIC)
endif()
add_library(aria::i18n ALIAS aria_i18n)

target_sources(aria_i18n PRIVATE
    src/locale_context.cpp
    src/text_bindings.cpp
    src/xml_catalog.cpp
)
target_include_directories(aria_i18n PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(aria_i18n PUBLIC cxx_std_23)
target_link_libraries(aria_i18n PUBLIC aria::core PRIVATE aria::warnings)
target_compile_definitions(aria_i18n PRIVATE ARIA_I18N_BUILD)
if(NOT ARIA_BUILD_SHARED)
    target_compile_definitions(aria_i18n PUBLIC ARIA_I18N_STATIC)
endif()
set_target_properties(aria_i18n PROPERTIES
    EXPORT_NAME i18n
    SOVERSION ${PROJECT_VERSION_MAJOR}
    VERSION ${PROJECT_VERSION}
)
if(ARIA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
