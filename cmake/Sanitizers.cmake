# Sanitizer configuration.
#
# Two ways to enable:
#   1) Individually:   -DARIA_ENABLE_ASAN=ON  (etc.)
#   2) One-shot alias: -DARIA_SANITIZE=asan   (asan|ubsan|tsan|msan|asan+ubsan|off)
#      -- convenience for CI pipelines, maps to the individual options below.

option(ARIA_ENABLE_ASAN  "Enable AddressSanitizer"                OFF)
option(ARIA_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer"      OFF)
option(ARIA_ENABLE_TSAN  "Enable ThreadSanitizer"                 OFF)
option(ARIA_ENABLE_MSAN  "Enable MemorySanitizer (Clang only)"    OFF)

set(ARIA_SANITIZE "" CACHE STRING
    "Shortcut: asan | ubsan | tsan | msan | asan+ubsan | off (empty disables)")

# Normalise the shortcut into the individual flags.
if(ARIA_SANITIZE)
    string(TOLOWER "${ARIA_SANITIZE}" _san_lc)
    if(_san_lc STREQUAL "off" OR _san_lc STREQUAL "none")
        # explicit disable — no-op
    elseif(_san_lc STREQUAL "asan")
        set(ARIA_ENABLE_ASAN ON CACHE BOOL "" FORCE)
    elseif(_san_lc STREQUAL "ubsan")
        set(ARIA_ENABLE_UBSAN ON CACHE BOOL "" FORCE)
    elseif(_san_lc STREQUAL "tsan")
        set(ARIA_ENABLE_TSAN ON CACHE BOOL "" FORCE)
    elseif(_san_lc STREQUAL "msan")
        set(ARIA_ENABLE_MSAN ON CACHE BOOL "" FORCE)
    elseif(_san_lc STREQUAL "asan+ubsan" OR _san_lc STREQUAL "ubsan+asan")
        set(ARIA_ENABLE_ASAN  ON CACHE BOOL "" FORCE)
        set(ARIA_ENABLE_UBSAN ON CACHE BOOL "" FORCE)
    else()
        message(FATAL_ERROR
            "ARIA_SANITIZE: unknown value '${ARIA_SANITIZE}'. "
            "Expected one of: asan | ubsan | tsan | msan | asan+ubsan | off")
    endif()
    message(STATUS "aria: sanitizer preset = ${ARIA_SANITIZE}")
endif()

function(aria_apply_sanitizers target)
    if(ARIA_ENABLE_TSAN AND (ARIA_ENABLE_ASAN OR ARIA_ENABLE_MSAN))
        message(FATAL_ERROR "TSan cannot be combined with ASan/MSan")
    endif()

    # MSVC sanitizer support is limited (`/fsanitize=address` only since
    # VS 2019 16.9; UBSan / TSan / MSan are not provided). We surface a
    # clear diagnostic instead of silently producing a binary without
    # the requested instrumentation.
    if(MSVC)
        if(ARIA_ENABLE_UBSAN)
            message(WARNING "ARIA_ENABLE_UBSAN: MSVC has no UBSan; ignoring on this toolchain.")
        endif()
        if(ARIA_ENABLE_TSAN)
            message(WARNING "ARIA_ENABLE_TSAN: MSVC has no ThreadSanitizer; ignoring on this toolchain.")
        endif()
        if(ARIA_ENABLE_MSAN)
            message(FATAL_ERROR "MSan requires Clang; not available on MSVC.")
        endif()
        if(ARIA_ENABLE_ASAN)
            target_compile_options(${target} INTERFACE /fsanitize=address)
            # /fsanitize=address on MSVC is incompatible with /RTC* and
            # /INCREMENTAL (linker). Drop them defensively.
            target_compile_options(${target} INTERFACE /Zi)
            target_link_options(${target} INTERFACE /INCREMENTAL:NO)
        endif()
        return()
    endif()

    set(_flags "")
    if(ARIA_ENABLE_ASAN)
        list(APPEND _flags -fsanitize=address -fno-omit-frame-pointer)
    endif()
    if(ARIA_ENABLE_UBSAN)
        list(APPEND _flags -fsanitize=undefined -fno-sanitize-recover=undefined)
    endif()
    if(ARIA_ENABLE_TSAN)
        list(APPEND _flags -fsanitize=thread)
    endif()
    if(ARIA_ENABLE_MSAN)
        if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            message(FATAL_ERROR "MSan requires Clang")
        endif()
        list(APPEND _flags -fsanitize=memory -fsanitize-memory-track-origins -fno-omit-frame-pointer)
    endif()

    if(_flags)
        target_compile_options(${target} INTERFACE ${_flags})
        target_link_options(${target} INTERFACE ${_flags})
    endif()
endfunction()
