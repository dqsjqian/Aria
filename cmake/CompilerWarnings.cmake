# Apply strict warning settings to a target.
#
# Aria treats every compiler warning as a hard build failure: the build
# tree is contractually warning-clean (see docs/api-style.md), and CI
# runs with `-Werror` / `/WX` to keep it that way. Enable
# `ARIA_WARNINGS_AS_ERRORS=OFF` only when an external toolchain
# upgrade introduces a new warning class that we have not yet
# audited — and that exception MUST be lifted by the next sprint.
option(ARIA_WARNINGS_AS_ERRORS
       "Treat compiler warnings as errors (CI default: ON)" ON)

function(aria_set_warnings target)
    set(MSVC_WARNINGS
        /W4
        /permissive-
        /EHsc
        /w14242 /w14254 /w14263 /w14265 /w14287 /we4289
        /w14296 /w14311 /w14545 /w14546 /w14547 /w14549
        /w14555 /w14619 /w14640 /w14826 /w14905 /w14906
        /w14928

        # Heavy template / coroutine code routinely overruns MSVC's
        # default 65535-symbol section limit; raising the limit avoids
        # spurious LNK1248-style failures in the binding library and
        # keeps debug builds linking cleanly.
        /bigobj

        # Make MSVC report __cplusplus correctly (default is 199711L
        # regardless of /std:c++20 — breaks libraries that use
        # __cplusplus to gate C++20 features).
        /Zc:__cplusplus

        # `noexcept` was treated as part of the type in C++17 — needed
        # for coroutine awaiter inheritance in libstdc++/libc++ headers
        # we test against in mixed configurations.
        # NOTE: the option is /Zc:noexceptTypes (MSVC 19.12+), NOT
        # /Zc:noexcept (which MSVC silently ignores with D9002).
        /Zc:noexceptTypes

        # MSVC defaults to the system code page (936/GBK on Chinese
        # Windows) for source file decoding. Aria's source files are
        # UTF-8 (some with BOM, some without, all containing Chinese
        # comments). Without /utf-8, MSVC emits C4819 "file contains
        # characters that cannot be represented in code page 936" for
        # every file — and under /WX that becomes a hard C2220 error.
        # /utf-8 sets both source and execution charset to UTF-8.
        /utf-8

        # `static_cast<int>(some_enum)` patterns in our constexpr
        # paths trip C4365 under /W4; we already opted into
        # /w14826 etc. above. No additional flags here.

        # C4251: "class needs to have dll-interface to be used by
        # clients of class" — fired whenever a dllexport'd class has
        # an STL member (std::unique_ptr, std::string, etc.).  This is
        # a fundamental C++/DLL limitation on Windows: STL types cannot
        # be exported across a DLL boundary, so the warning fires on
        # every non-trivial exported class.  Universally silenced in
        # real-world MSVC C++ projects (Qt, Boost, etc. all do this).
        /wd4251

        # C5285: "cannot specialize standard library template" — fires
        # on third_party/doctest/doctest.h(539) where doctest specializes
        # std::tuple.  This is a doctest issue, not ours; we cannot fix
        # the vendored doctest source.  Silenced to keep VS 2026 / MSVC
        # 19.5x builds green (the warning is also /Wv:18-gated, so older
        # MSVC versions are unaffected).
        /wd5285

        # NOTE on C4619 ("#pragma warning: there is no warning number
        # '4865'", raised by doctest 2.5.3's VS-2026-targeted pragma on
        # MSVC 2022): do NOT try to silence it here — /wd4619 has no
        # effect because the warning is emitted while __pragma() is being
        # evaluated, before command-line warning filters apply.  The fix
        # is a _MSC_VER >= 1945 guard patched directly into the vendored
        # doctest.h (search "[ARIA PATCH]").
    )

    set(CLANG_WARNINGS
        -Wall
        -Wextra
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wnull-dereference
        -Wdouble-promotion
        -Wformat=2
        -Wno-unused-parameter
        -Wno-unused-private-field
    )

    # -Wnull-dereference omitted for GCC: 15.2+ generates excessive
    # false positives on shared_ptr internals inlined through template
    # expansion (PagedList, AsyncCommandCore etc.). Clang's analysis is
    # more precise — keep it there.
    set(GCC_WARNINGS
        -Wall
        -Wextra
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wdouble-promotion
        -Wformat=2
        -Wno-unused-parameter
        -Wmisleading-indentation
        -Wduplicated-cond
        -Wduplicated-branches
        -Wlogical-op
        -Wuseless-cast
    )

    if(MSVC)
        target_compile_options(${target} INTERFACE ${MSVC_WARNINGS})
        # Reduce stdlib header noise on MSVC: external/system include
        # warnings are toned down so our /W4 doesn't drown in
        # unrelated MS-CRT warnings.
        target_compile_options(${target} INTERFACE
            $<$<COMPILE_LANGUAGE:CXX>:/external:W0>
        )
        if(ARIA_WARNINGS_AS_ERRORS)
            target_compile_options(${target} INTERFACE /WX)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${target} INTERFACE ${CLANG_WARNINGS})
        if(ARIA_WARNINGS_AS_ERRORS)
            target_compile_options(${target} INTERFACE -Werror)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${target} INTERFACE ${GCC_WARNINGS})
        if(ARIA_WARNINGS_AS_ERRORS)
            target_compile_options(${target} INTERFACE -Werror)
        endif()
    endif()
endfunction()
