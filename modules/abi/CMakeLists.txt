add_library(aria_abi STATIC
    src/signal.cpp
    src/slot.cpp
    src/callback_boundary.cpp
)
add_library(aria::abi ALIAS aria_abi)

target_include_directories(aria_abi PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(aria_abi PUBLIC cxx_std_20)

# Build-time marker: turns ARIA_ABI_API into ARIA_EXPORT inside this TU.
# In the current STATIC build it stays empty (because ARIA_ABI_STATIC wins
# at the consumer side); but defining it PRIVATE here keeps the macro
# semantics correct should aria_abi ever switch to SHARED.
target_compile_definitions(aria_abi PRIVATE ARIA_ABI_BUILD)

# On Windows, abi is a static library but other modules may expect dllimport.
# Define ARIA_ABI_STATIC so headers don't use dllimport/dllexport for abi symbols.
if(WIN32)
    target_compile_definitions(aria_abi PUBLIC ARIA_ABI_STATIC)
endif()

target_link_libraries(aria_abi
    PUBLIC aria::core Threads::Threads
    PRIVATE aria::warnings
)

# When statically linked into another shared lib, abi must be PIC
set_target_properties(aria_abi PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(ARIA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
