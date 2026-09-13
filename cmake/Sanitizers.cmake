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
    "Shortcut: asan | ubsan | tsan | msan | asan+ubsan | off (empty uses individual options)")

# Normalise the shortcut into the individual flags.
if(ARIA_SANITIZE)
    string(TOLOWER "${ARIA_SANITIZE}" _san_lc)
    # A preset describes the complete configuration, including when the
    # same build directory switches from one preset to another.
    foreach(_san ASAN UBSAN TSAN MSAN)
        set(ARIA_ENABLE_${_san} OFF CACHE BOOL "" FORCE)
    endforeach()
    if(_san_lc STREQUAL "off" OR _san_lc STREQUAL "none")
        # All individual switches were cleared above.
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

# Keep the original cache values intact: switching this directory back to a
# normal Debug build must restore its runtime checks and debug format.
if(MSVC AND ARIA_ENABLE_ASAN)
    function(_aria_msvc_asan_flags output flags)
        set(_result "")
        set(_token "")
        set(_quoted FALSE)
        set(_backslashes 0)
        string(LENGTH "${flags}" _length)
        set(_index 0)
        while(_index LESS _length)
            string(SUBSTRING "${flags}" ${_index} 1 _char)
            if(_char STREQUAL "\"")
                math(EXPR _escaped "${_backslashes} % 2")
                if(NOT _escaped)
                    if(_quoted)
                        set(_quoted FALSE)
                    else()
                        set(_quoted TRUE)
                    endif()
                endif()
            endif()
            if(NOT _quoted AND _char MATCHES "[ \t]")
                # Match complete arguments, including quoted arguments. A
                # /D definition containing the text /RTC1 is not an option.
                if(NOT _token MATCHES [=[^"?[-/](RTC[1csu]+|ZI)"?$]=])
                    string(APPEND _result "${_token}")
                endif()
                string(APPEND _result "${_char}")
                set(_token "")
            else()
                string(APPEND _token "${_char}")
            endif()
            if(_char STREQUAL "\\")
                math(EXPR _backslashes "${_backslashes} + 1")
            else()
                set(_backslashes 0)
            endif()
            math(EXPR _index "${_index} + 1")
        endwhile()
        if(NOT _token MATCHES [=[^"?[-/](RTC[1csu]+|ZI)"?$]=])
            string(APPEND _result "${_token}")
        endif()
        set(${output} "${_result}" PARENT_SCOPE)
    endfunction()

    # Include custom configuration names, not just CMake's standard four.
    get_cmake_property(_aria_variables VARIABLES)
    foreach(_aria_variable IN LISTS _aria_variables)
        if(_aria_variable MATCHES "^CMAKE_(C|CXX|OBJC|OBJCXX)_FLAGS($|_)")
            _aria_msvc_asan_flags(${_aria_variable} "${${_aria_variable}}")
        endif()
    endforeach()
    unset(_aria_variable)
    unset(_aria_variables)

    # Newer CMake versions can express these options through properties
    # instead of flag strings. These ordinary variables are harmless on 3.20.
    set(CMAKE_MSVC_RUNTIME_CHECKS "")
    if(DEFINED CMAKE_MSVC_DEBUG_INFORMATION_FORMAT)
        string(REPLACE "EditAndContinue" "ProgramDatabase"
            CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "${CMAKE_MSVC_DEBUG_INFORMATION_FORMAT}")
    endif()
endif()

function(aria_apply_sanitizers target)
    if(ARIA_ENABLE_ASAN AND ARIA_ENABLE_MSAN)
        message(FATAL_ERROR "ASan cannot be combined with MSan")
    endif()
    if(ARIA_ENABLE_TSAN AND (ARIA_ENABLE_ASAN OR ARIA_ENABLE_MSAN))
        message(FATAL_ERROR "TSan cannot be combined with ASan/MSan")
    endif()

    # MSVC sanitizer support is limited (`/fsanitize=address` only since
    # VS 2019 16.9; UBSan / TSan / MSan are not provided). We surface a
    # clear diagnostic instead of silently producing a binary without
    # the requested instrumentation.
    if(MSVC)
        if(ARIA_ENABLE_UBSAN)
            message(FATAL_ERROR "ARIA_ENABLE_UBSAN: MSVC has no UBSan.")
        endif()
        if(ARIA_ENABLE_TSAN)
            message(FATAL_ERROR "ARIA_ENABLE_TSAN: MSVC has no ThreadSanitizer.")
        endif()
        if(ARIA_ENABLE_MSAN)
            message(FATAL_ERROR "MSan requires Clang; not available on MSVC.")
        endif()
        if(ARIA_ENABLE_ASAN)
            target_compile_options(${target} INTERFACE /fsanitize=address)
            # /RTC* and /ZI were removed above. /Zi is supported by ASan;
            # incremental linking must also be disabled.
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
