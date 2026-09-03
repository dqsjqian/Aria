// =============================================================
// == DO NOT MODIFY THIS FILE BY HAND - IT IS AUTO GENERATED! ==
// =============================================================
//
// doctest.h - the lightest feature-rich C++ single-header testing framework for unit tests and TDD
//
// Copyright (c) 2016-2023 Viktor Kirilov
//
// Distributed under the MIT Software License
// See accompanying file LICENSE.txt or copy at
// https://opensource.org/licenses/MIT
//
// The documentation can be found at the library's page:
// https://github.com/doctest/doctest/blob/master/doc/markdown/readme.md
//
// =================================================================================================
// =================================================================================================
// =================================================================================================
//
// The library is heavily influenced by Catch - https://github.com/catchorg/Catch2
// which uses the Boost Software License - Version 1.0
// see here - https://github.com/catchorg/Catch2/blob/master/LICENSE.txt
//
// The concept of subcases (sections in Catch) and expression decomposition are from there.
// Some parts of the code are taken directly:
// - stringification - the detection of "ostream& operator<<(ostream&, const T&)" and StringMaker<>
// - the Approx() helper class for floating point comparison
// - colors in the console
// - breaking into a debugger
// - signal / SEH handling
// - timer
// - XmlWriter class - thanks to Phil Nash for allowing the direct reuse (AKA copy/paste)
//
// The expression decomposing templates are taken from lest - https://github.com/martinmoene/lest
// which uses the Boost Software License - Version 1.0
// see here - https://github.com/martinmoene/lest/blob/master/LICENSE.txt
//
// =================================================================================================
// =================================================================================================
// =================================================================================================

#ifndef DOCTEST_LIBRARY_INCLUDED
#define DOCTEST_LIBRARY_INCLUDED

// =================================================================================================
// == VERSION ======================================================================================
// =================================================================================================

#ifndef DOCTEST_PARTS_PUBLIC_VERSION
#define DOCTEST_PARTS_PUBLIC_VERSION

// NOLINTBEGIN(cppcoreguidelines-macro-to-enum, modernize-macro-to-enum)
#define DOCTEST_VERSION_MAJOR 2
#define DOCTEST_VERSION_MINOR 5
#define DOCTEST_VERSION_PATCH 3
// NOLINTEND(cppcoreguidelines-macro-to-enum, modernize-macro-to-enum)

// util we need here
#define DOCTEST_TOSTR_IMPL(x) #x
#define DOCTEST_TOSTR(x) DOCTEST_TOSTR_IMPL(x)

// clang-format off
#define DOCTEST_VERSION_STR                                                                                            \
    DOCTEST_TOSTR(DOCTEST_VERSION_MAJOR) "."                                                                           \
    DOCTEST_TOSTR(DOCTEST_VERSION_MINOR) "."                                                                           \
    DOCTEST_TOSTR(DOCTEST_VERSION_PATCH)
// clang-format on

#define DOCTEST_VERSION (DOCTEST_VERSION_MAJOR * 10000 + DOCTEST_VERSION_MINOR * 100 + DOCTEST_VERSION_PATCH)

#endif // DOCTEST_PARTS_PUBLIC_VERSION
// =================================================================================================
// == COMPILER VERSION =============================================================================
// =================================================================================================

#ifndef DOCTEST_PARTS_PUBLIC_COMPILER
#define DOCTEST_PARTS_PUBLIC_COMPILER

// ideas for the version stuff are taken from here: https://github.com/cxxstuff/cxx_detect

#ifdef _MSC_VER
#define DOCTEST_CPLUSPLUS _MSVC_LANG
#else
#define DOCTEST_CPLUSPLUS __cplusplus
#endif

#define DOCTEST_COMPILER(MAJOR, MINOR, PATCH) ((MAJOR) * 10000000 + (MINOR) * 100000 + (PATCH))

// GCC/Clang and GCC/MSVC are mutually exclusive, but Clang/MSVC are not because of clang-cl...
#if defined(_MSC_VER) && defined(_MSC_FULL_VER)
#if _MSC_VER == _MSC_FULL_VER / 10000
#define DOCTEST_MSVC DOCTEST_COMPILER(_MSC_VER / 100, _MSC_VER % 100, _MSC_FULL_VER % 10000)
#else // MSVC
#define DOCTEST_MSVC DOCTEST_COMPILER(_MSC_VER / 100, (_MSC_FULL_VER / 100000) % 100, _MSC_FULL_VER % 100000)
#endif // MSVC
#endif // MSVC
#if defined(__clang__) && defined(__clang_minor__) && defined(__clang_patchlevel__)
#define DOCTEST_CLANG DOCTEST_COMPILER(__clang_major__, __clang_minor__, __clang_patchlevel__)
#elif defined(__GNUC__) && defined(__GNUC_MINOR__) && defined(__GNUC_PATCHLEVEL__) && !defined(__INTEL_COMPILER)
#define DOCTEST_GCC DOCTEST_COMPILER(__GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__)
#endif // GCC
#if defined(__INTEL_COMPILER)
#define DOCTEST_ICC DOCTEST_COMPILER(__INTEL_COMPILER / 100, __INTEL_COMPILER % 100, 0)
#endif // ICC

#ifndef DOCTEST_MSVC
#define DOCTEST_MSVC 0
#endif // DOCTEST_MSVC
#ifndef DOCTEST_CLANG
#define DOCTEST_CLANG 0
#endif // DOCTEST_CLANG
#ifndef DOCTEST_GCC
#define DOCTEST_GCC 0
#endif // DOCTEST_GCC
#ifndef DOCTEST_ICC
#define DOCTEST_ICC 0
#endif // DOCTEST_ICC

#endif // DOCTEST_PARTS_PUBLIC_COMPILER
// =================================================================================================
// == COMPILER WARNINGS HELPERS ====================================================================
// =================================================================================================

#ifndef DOCTEST_PARTS_PUBLIC_WARNINGS
#define DOCTEST_PARTS_PUBLIC_WARNINGS


#if DOCTEST_CLANG && !DOCTEST_ICC
#define DOCTEST_PRAGMA_TO_STR(x) _Pragma(#x)
#define DOCTEST_CLANG_SUPPRESS_WARNING_PUSH _Pragma("clang diagnostic push")
#define DOCTEST_CLANG_SUPPRESS_WARNING(w) DOCTEST_PRAGMA_TO_STR(clang diagnostic ignored w)
#define DOCTEST_CLANG_SUPPRESS_WARNING_POP _Pragma("clang diagnostic pop")
#define DOCTEST_CLANG_SUPPRESS_WARNING_WITH_PUSH(w)                                                                    \
    DOCTEST_CLANG_SUPPRESS_WARNING_PUSH DOCTEST_CLANG_SUPPRESS_WARNING(w)
#else // DOCTEST_CLANG
#define DOCTEST_CLANG_SUPPRESS_WARNING_PUSH
#define DOCTEST_CLANG_SUPPRESS_WARNING(w)
#define DOCTEST_CLANG_SUPPRESS_WARNING_POP
#define DOCTEST_CLANG_SUPPRESS_WARNING_WITH_PUSH(w)
#endif // DOCTEST_CLANG

#if DOCTEST_GCC
#define DOCTEST_PRAGMA_TO_STR(x) _Pragma(#x)
#define DOCTEST_GCC_SUPPRESS_WARNING_PUSH _Pragma("GCC diagnostic push")
#define DOCTEST_GCC_SUPPRESS_WARNING(w) DOCTEST_PRAGMA_TO_STR(GCC diagnostic ignored w)
#define DOCTEST_GCC_SUPPRESS_WARNING_POP _Pragma("GCC diagnostic pop")
#define DOCTEST_GCC_SUPPRESS_WARNING_WITH_PUSH(w) DOCTEST_GCC_SUPPRESS_WARNING_PUSH DOCTEST_GCC_SUPPRESS_WARNING(w)
#else // DOCTEST_GCC
#define DOCTEST_GCC_SUPPRESS_WARNING_PUSH
#define DOCTEST_GCC_SUPPRESS_WARNING(w)
#define DOCTEST_GCC_SUPPRESS_WARNING_POP
#define DOCTEST_GCC_SUPPRESS_WARNING_WITH_PUSH(w)
#endif // DOCTEST_GCC

#if DOCTEST_MSVC
#define DOCTEST_MSVC_SUPPRESS_WARNING_PUSH __pragma(warning(push))
#define DOCTEST_MSVC_SUPPRESS_WARNING(w) __pragma(warning(disable : w))
#define DOCTEST_MSVC_SUPPRESS_WARNING_POP __pragma(warning(pop))
#define DOCTEST_MSVC_SUPPRESS_WARNING_WITH_PUSH(w) DOCTEST_MSVC_SUPPRESS_WARNING_PUSH DOCTEST_MSVC_SUPPRESS_WARNING(w)
#else // DOCTEST_MSVC
#define DOCTEST_MSVC_SUPPRESS_WARNING_PUSH
#define DOCTEST_MSVC_SUPPRESS_WARNING(w)
#define DOCTEST_MSVC_SUPPRESS_WARNING_POP
#define DOCTEST_MSVC_SUPPRESS_WARNING_WITH_PUSH(w)
#endif // DOCTEST_MSVC

// =================================================================================================
// == COMPILER WARNINGS ============================================================================
// =================================================================================================

// both the header and the implementation suppress all of these,
// so it only makes sense to aggregate them like so
#define DOCTEST_SUPPRESS_COMMON_WARNINGS_PUSH                                                                          \
    DOCTEST_CLANG_SUPPRESS_WARNING_PUSH                                                                                \
    DOCTEST_CLANG_SUPPRESS_WARNING("-Wunknown-pragmas")                                                                \
    DOCTEST_CLANG_SUPPRESS_WARNING("-Wunknown-warning-option")                                                         \
    DOCTEST_CLANG_SUPPRESS_WARNING("-Wweak-vtables")                                                                   \
    DOCTEST_CLANG_SUPPRESS_WARNING("-Wpadded")                                                                         \
    DOCTEST_CLANG_SUPPRESS_WARNING("-Wmissing-prototypes")                                                             \
    DOCTEST_CLANG_SUPPRESS_WARNING("-Wc++98-compat")                                                                   \
    DOCTEST_CLANG_SUPPRESS_WARNING("-Wc++98-compat-pedantic")                                                          \
    DOCTEST_CLANG_SUPPRESS_WARNING("-Wunsafe-buffer-usage")                                                            \
    DOCTEST_CLANG_SUPPRESS_WARNING("-Wunused-macros")                                                                  \
                                                                                                                       \
    DOCTEST_GCC_SUPPRESS_WARNING_PUSH                                                                                  \
    DOCTEST_GCC_SUPPRESS_WARNING("-Wunknown-pragmas")                                                                  \
    DOCTEST_GCC_SUPPRESS_WARNING("-Wpragmas")                                                                          \
    DOCTEST_GCC_SUPPRESS_WARNING("-Weffc++")                                                                           \
    DOCTEST_GCC_SUPPRESS_WARNING("-Wstrict-overflow")                                                                  \
    DOCTEST_GCC_SUPPRESS_WARNING("-Wstrict-aliasing")                                                                  \
    DOCTEST_GCC_SUPPRESS_WARNING("-Wmissing-declarations")                                                             \
    DOCTEST_GCC_SUPPRESS_WARNING("-Wuseless-cast")                                                                     \
    DOCTEST_GCC_SUPPRESS_WARNING("-Wnoexcept")                                                                         \
                                                                                                                       \
    DOCTEST_MSVC_SUPPRESS_WARNING_PUSH                                                                                 \
    /* these 4 also disabled globally via cmake: */                                                                    \
    DOCTEST_MSVC_SUPPRESS_WARNING(4514) /* unreferenced inline function has been removed */                            \
    DOCTEST_MSVC_SUPPRESS_WARNING(4571) /* SEH related */                                                              \
    DOCTEST_MSVC_SUPPRESS_WARNING(4710) /* function not inlined */                                                     \
    DOCTEST_MSVC_SUPPRESS_WARNING(4711) /* function selected for inline expansion*/                                    \
    /* common ones */                                                                                                  \
    DOCTEST_MSVC_SUPPRESS_WARNING(4616) /* invalid compiler warning */                                                 \
    DOCTEST_MSVC_SUPPRESS_WARNING(4619) /* invalid compiler warning */                                                 \
    DOCTEST_MSVC_SUPPRESS_WARNING(4996) /* The compiler encountered a deprecated declaration */                        \
    DOCTEST_MSVC_SUPPRESS_WARNING(4706) /* assignment within conditional expression */                                 \
    DOCTEST_MSVC_SUPPRESS_WARNING(4512) /* 'class' : assignment operator could not be generated */                     \
    DOCTEST_MSVC_SUPPRESS_WARNING(4127) /* conditional expression is constant */                                       \
    DOCTEST_MSVC_SUPPRESS_WARNING(4820) /* padding */                                                                  \
    DOCTEST_MSVC_SUPPRESS_WARNING(4625) /* copy constructor was implicitly deleted */                                  \
    DOCTEST_MSVC_SUPPRESS_WARNING(4626) /* assignment operator was implicitly deleted */                               \
    DOCTEST_MSVC_SUPPRESS_WARNING(5027) /* move assignment operator implicitly deleted */                              \
    DOCTEST_MSVC_SUPPRESS_WARNING(5026) /* move constructor was implicitly deleted */                                  \
    DOCTEST_MSVC_SUPPRESS_WARNING(4640) /* construction of local static object not thread-safe */                      \
    DOCTEST_MSVC_SUPPRESS_WARNING(5045) /* Spectre mitigation for memory load */                                       \
    DOCTEST_MSVC_SUPPRESS_WARNING(5264) /* 'variable-name': 'const' variable is not used */                            \
    /* static analysis */                                                                                              \
    DOCTEST_MSVC_SUPPRESS_WARNING(26439) /* Function may not throw. Declare it 'noexcept' */                           \
    DOCTEST_MSVC_SUPPRESS_WARNING(26495) /* Always initialize a member variable */                                     \
    DOCTEST_MSVC_SUPPRESS_WARNING(26451) /* Arithmetic overflow ... */                                                 \
    DOCTEST_MSVC_SUPPRESS_WARNING(26444) /* Avoid unnamed objects with custom ctor and dtor... */                      \
    DOCTEST_MSVC_SUPPRESS_WARNING(26812) /* Prefer 'enum class' over 'enum' */

#define DOCTEST_SUPPRESS_COMMON_WARNINGS_POP                                                                           \
    DOCTEST_CLANG_SUPPRESS_WARNING_POP                                                                                 \
    DOCTEST_GCC_SUPPRESS_WARNING_POP                                                                                   \
    DOCTEST_MSVC_SUPPRESS_WARNING_POP

#define DOCTEST_SUPPRESS_PUBLIC_WARNINGS_PUSH                                                                          \
    DOCTEST_SUPPRESS_COMMON_WARNINGS_PUSH                                                                              \
                                                                                                                       \
    DOCTEST_CLANG_SUPPRESS_WARNING("-Wnon-virtual-dtor")                                                               \
    DOCTEST_CLANG_SUPPRESS_WARNING("-Wdeprecated")                                                                     \
                                                                                                                       \
    DOCTEST_GCC_SUPPRESS_WARNING("-Wctor-dtor-privacy")                                                                \
    DOCTEST_GCC_SUPPRESS_WARNING("-Wnon-virtual-dtor")                                                                 \
    DOCTEST_GCC_SUPPRESS_WARNING("-Wsign-promo")                                                                       \
                                                                                                                       \
    DOCTEST_MSVC_SUPPRESS_WARNING(4623) /* default constructor was implicitly deleted */

#define DOCTEST_SUPPRESS_PUBLIC_WARNINGS_POP DOCTEST_SUPPRESS_COMMON_WARNINGS_POP

#define DOCTEST_SUPPRESS_PRIVATE_WARNINGS_PUSH                                                                         \
    DOCTEST_SUPPRESS_COMMON_WARNINGS_PUSH                                                                              \
                                                                                                                       \
    DOCTEST_CLANG_SUPPRESS_WARNING("-Wglobal-constructors")                                                            \
    DOCTEST_CLANG_SUPPRESS_WARNING("-Wexit-time-destructors")                                                          \
    DOCTEST_CLANG_SUPPRESS_WARNING("-Wsign-conversion")                                                                \
    DOCTEST_CLANG_SUPPRESS_WARNING("-Wshorten-64-to-32")                                                               \
    DOCTEST_CLANG_SUPPRESS_WARNING("-Wmissing-variable-declarations")                                                  \
    DOCTEST_CLANG_SUPPRESS_WARNING("-Wswitch")                                                                         \
    DOCTEST_CLANG_SUPPRESS_WARNING("-Wswitch-enum")                                                                    \
    DOCTEST_CLANG_SUPPRESS_WARNING("-Wcovered-switch-default")                                                         \
    DOCTEST_CLANG_SUPPRESS_WARNING("-Wmissing-noreturn")                                                               \
    DOCTEST_CLANG_SUPPRESS_WARNING("-Wdisabled-macro-expansion")                                                       \
    DOCTEST_CLANG_SUPPRESS_WARNING("-Wmissing-braces")                                                                 \
    DOCTEST_CLANG_SUPPRESS_WARNING("-Wmissing-field-initializers")                                                     \
    DOCTEST_CLANG_SUPPRESS_WARNING("-Wunused-member-function")                                                         \
    DOCTEST_CLANG_SUPPRESS_WARNING("-Wunused-function")                                                                \
    DOCTEST_CLANG_SUPPRESS_WARNING("-Wnrvo")                                                                           \
                                                                                                                       \
    DOCTEST_GCC_SUPPRESS_WARNING("-Wconversion")                                                                       \
    DOCTEST_GCC_SUPPRESS_WARNING("-Wsign-conversion")                                                                  \
    DOCTEST_GCC_SUPPRESS_WARNING("-Wmissing-field-initializers")                                                       \
    DOCTEST_GCC_SUPPRESS_WARNING("-Wmissing-braces")                                                                   \
    DOCTEST_GCC_SUPPRESS_WARNING("-Wswitch")                                                                           \
    DOCTEST_GCC_SUPPRESS_WARNING("-Wswitch-enum")                                                                      \
    DOCTEST_GCC_SUPPRESS_WARNING("-Wswitch-default")                                                                   \
    DOCTEST_GCC_SUPPRESS_WARNING("-Wunsafe-loop-optimizations")                                                        \
    DOCTEST_GCC_SUPPRESS_WARNING("-Wold-style-cast")                                                                   \
    DOCTEST_GCC_SUPPRESS_WARNING("-Wunused-function")                                                                  \
    DOCTEST_GCC_SUPPRESS_WARNING("-Wmultiple-inheritance")                                                             \
    DOCTEST_GCC_SUPPRESS_WARNING("-Wsuggest-attribute")                                                                \
    DOCTEST_GCC_SUPPRESS_WARNING("-Wnrvo")                                                                             \
                                                                                                                       \
    DOCTEST_MSVC_SUPPRESS_WARNING(4267) /* conversion from 'x' to 'y', possible loss of data */                        \
    DOCTEST_MSVC_SUPPRESS_WARNING(4530) /* exception handler, but unwind semantics not enabled */                      \
    DOCTEST_MSVC_SUPPRESS_WARNING(4577) /* 'noexcept' with no exception handling mode specified */                     \
    DOCTEST_MSVC_SUPPRESS_WARNING(4774) /* format string in argument is not a string literal */                        \
    DOCTEST_MSVC_SUPPRESS_WARNING(4365) /* signed/unsigned mismatch */                                                 \
    DOCTEST_MSVC_SUPPRESS_WARNING(5039) /* pointer to pot. throwing function passed to extern C */                     \
    DOCTEST_MSVC_SUPPRESS_WARNING(4800) /* forcing value to bool (performance warning) */                              \
    DOCTEST_MSVC_SUPPRESS_WARNING(5245) /* unreferenced function with internal linkage removed */

#define DOCTEST_SUPPRESS_PRIVATE_WARNINGS_POP DOCTEST_SUPPRESS_COMMON_WARNINGS_POP

#endif // DOCTEST_PARTS_PUBLIC_WARNINGS

DOCTEST_SUPPRESS_PUBLIC_WARNINGS_PUSH

// =================================================================================================
// == FEATURE DETECTION ============================================================================
// =================================================================================================

#ifndef DOCTEST_PARTS_PUBLIC_CONFIG
#define DOCTEST_PARTS_PUBLIC_CONFIG

#ifndef DOCTEST_PARTS_PUBLIC_PLATFORM
#define DOCTEST_PARTS_PUBLIC_PLATFORM

#if defined(__APPLE__)
// Apple detection taken from Catch2 codebase
// For <TargetConditionals.h> information:
//   https://github.com/swiftlang/swift-corelibs-foundation/blob/release/5.10/CoreFoundation/Base.subproj/SwiftRuntime/TargetConditionals.h
#include <TargetConditionals.h>
#if (defined(TARGET_OS_MAC) && TARGET_OS_MAC == 1) || (defined(TARGET_OS_OSX) && TARGET_OS_OSX == 1)
#define DOCTEST_PLATFORM_MAC

#elif defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE == 1
#define DOCTEST_PLATFORM_IPHONE
#endif

#elif defined(WIN32) || defined(_WIN32)
#define DOCTEST_PLATFORM_WINDOWS

#elif defined(__wasi__)
#define DOCTEST_PLATFORM_WASI

#else // defined(linux) || defined(__linux) // defined(__linux__)
#define DOCTEST_PLATFORM_LINUX
#endif // DOCTEST_PLATFORM

#endif // DOCTEST_PARTS_PUBLIC_PLATFORM

// general compiler feature support table: https://en.cppreference.com/w/cpp/compiler_support
// MSVC C++11 feature support table: https://msdn.microsoft.com/en-us/library/hh567368.aspx
// GCC C++11 feature support table: https://gcc.gnu.org/projects/cxx-status.html
// MSVC version table:
// https://en.wikipedia.org/wiki/Microsoft_Visual_C%2B%2B#Internal_version_numbering
// MSVC++ 14.3 (17) _MSC_VER == 1930 (Visual Studio 2022)
// MSVC++ 14.2 (16) _MSC_VER == 1920 (Visual Studio 2019)
// MSVC++ 14.1 (15) _MSC_VER == 1910 (Visual Studio 2017)
// MSVC++ 14.0      _MSC_VER == 1900 (Visual Studio 2015)
// MSVC++ 12.0      _MSC_VER == 1800 (Visual Studio 2013)
// MSVC++ 11.0      _MSC_VER == 1700 (Visual Studio 2012)
// MSVC++ 10.0      _MSC_VER == 1600 (Visual Studio 2010)
// MSVC++ 9.0       _MSC_VER == 1500 (Visual Studio 2008)
// MSVC++ 8.0       _MSC_VER == 1400 (Visual Studio 2005)

// Universal Windows Platform support
#if defined(WINAPI_FAMILY) && (WINAPI_FAMILY == WINAPI_FAMILY_APP)
#ifndef DOCTEST_CONFIG_NO_WINDOWS_SEH
#define DOCTEST_CONFIG_NO_WINDOWS_SEH
#endif
#ifndef DOCTEST_CONFIG_NO_MULTI_LANE_ATOMICS
#define DOCTEST_CONFIG_NO_MULTI_LANE_ATOMICS
#endif
#endif // defined(WINAPI_FAMILY) && (WINAPI_FAMILY == WINAPI_FAMILY_APP)
#if DOCTEST_MSVC && !defined(DOCTEST_CONFIG_WINDOWS_SEH)
#define DOCTEST_CONFIG_WINDOWS_SEH
#endif // DOCTEST_MSVC && !defined(DOCTEST_CONFIG_WINDOWS_SEH)
#if defined(DOCTEST_CONFIG_NO_WINDOWS_SEH) && defined(DOCTEST_CONFIG_WINDOWS_SEH)
#undef DOCTEST_CONFIG_WINDOWS_SEH
#endif // defined(DOCTEST_CONFIG_NO_WINDOWS_SEH) && defined(DOCTEST_CONFIG_WINDOWS_SEH)

#if !defined(_WIN32) && !defined(__QNX__) && !defined(DOCTEST_CONFIG_POSIX_SIGNALS) && !defined(__EMSCRIPTEN__) &&     \
    !defined(__wasi__)
#define DOCTEST_CONFIG_POSIX_SIGNALS
#endif // _WIN32
#if defined(DOCTEST_CONFIG_NO_POSIX_SIGNALS) && defined(DOCTEST_CONFIG_POSIX_SIGNALS)
#undef DOCTEST_CONFIG_POSIX_SIGNALS
#endif // DOCTEST_CONFIG_NO_POSIX_SIGNALS

#ifndef DOCTEST_CONFIG_NO_EXCEPTIONS
#if !defined(__cpp_exceptions) && !defined(__EXCEPTIONS) && !defined(_CPPUNWIND) || defined(__wasi__)
#define DOCTEST_CONFIG_NO_EXCEPTIONS
#endif // no exceptions
#endif // DOCTEST_CONFIG_NO_EXCEPTIONS

#ifdef DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#ifndef DOCTEST_CONFIG_NO_EXCEPTIONS
#define DOCTEST_CONFIG_NO_EXCEPTIONS
#endif // DOCTEST_CONFIG_NO_EXCEPTIONS
#endif // DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS

#if defined(DOCTEST_CONFIG_NO_EXCEPTIONS) && !defined(DOCTEST_CONFIG_NO_TRY_CATCH_IN_ASSERTS)
#define DOCTEST_CONFIG_NO_TRY_CATCH_IN_ASSERTS
#endif // DOCTEST_CONFIG_NO_EXCEPTIONS && !DOCTEST_CONFIG_NO_TRY_CATCH_IN_ASSERTS

#ifdef __wasi__
#define DOCTEST_CONFIG_NO_MULTITHREADING
#endif

#if defined(DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN) && !defined(DOCTEST_CONFIG_IMPLEMENT)
#define DOCTEST_CONFIG_IMPLEMENT
#endif // DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#if defined(_WIN32) || defined(__CYGWIN__)
#if DOCTEST_MSVC
#define DOCTEST_SYMBOL_EXPORT __declspec(dllexport)
#define DOCTEST_SYMBOL_IMPORT __declspec(dllimport)
#else // MSVC
#define DOCTEST_SYMBOL_EXPORT __attribute__((dllexport))
#define DOCTEST_SYMBOL_IMPORT __attribute__((dllimport))
#endif // MSVC
#else  // _WIN32
#define DOCTEST_SYMBOL_EXPORT __attribute__((visibility("default")))
#define DOCTEST_SYMBOL_IMPORT
#endif // _WIN32

#ifdef DOCTEST_CONFIG_IMPLEMENTATION_IN_DLL
#ifdef DOCTEST_CONFIG_IMPLEMENT
#define DOCTEST_INTERFACE DOCTEST_SYMBOL_EXPORT
#else // DOCTEST_CONFIG_IMPLEMENT
#define DOCTEST_INTERFACE DOCTEST_SYMBOL_IMPORT
#endif // DOCTEST_CONFIG_IMPLEMENT
#else  // DOCTEST_CONFIG_IMPLEMENTATION_IN_DLL
#define DOCTEST_INTERFACE
#endif // DOCTEST_CONFIG_IMPLEMENTATION_IN_DLL

// needed for extern template instantiations
// see https://github.com/fmtlib/fmt/issues/2228
#if DOCTEST_MSVC
#define DOCTEST_INTERFACE_DECL
#define DOCTEST_INTERFACE_DEF DOCTEST_INTERFACE
#else // DOCTEST_MSVC
#define DOCTEST_INTERFACE_DECL DOCTEST_INTERFACE
#define DOCTEST_INTERFACE_DEF
#endif // DOCTEST_MSVC

#define DOCTEST_EMPTY

#if DOCTEST_MSVC
#define DOCTEST_NOINLINE __declspec(noinline)
#define DOCTEST_UNUSED
#define DOCTEST_ALIGNMENT(x)
#elif DOCTEST_CLANG && DOCTEST_CLANG < DOCTEST_COMPILER(3, 5, 0)
#define DOCTEST_NOINLINE
#define DOCTEST_UNUSED
#define DOCTEST_ALIGNMENT(x)
#else
#define DOCTEST_NOINLINE __attribute__((noinline))
#define DOCTEST_UNUSED __attribute__((unused))
#define DOCTEST_ALIGNMENT(x) __attribute__((aligned(x)))
#endif

#ifdef DOCTEST_CONFIG_NO_CONTRADICTING_INLINE
#define DOCTEST_INLINE_NOINLINE inline
#else
#define DOCTEST_INLINE_NOINLINE inline DOCTEST_NOINLINE
#endif

#ifndef DOCTEST_NORETURN
#if DOCTEST_MSVC && (DOCTEST_MSVC < DOCTEST_COMPILER(19, 0, 0))
#define DOCTEST_NORETURN
#else // DOCTEST_MSVC
#define DOCTEST_NORETURN [[noreturn]]
#endif // DOCTEST_MSVC
#endif // DOCTEST_NORETURN

#ifndef DOCTEST_NOEXCEPT
#if DOCTEST_MSVC && (DOCTEST_MSVC < DOCTEST_COMPILER(19, 0, 0))
#define DOCTEST_NOEXCEPT
#else // DOCTEST_MSVC
#define DOCTEST_NOEXCEPT noexcept
#endif // DOCTEST_MSVC
#endif // DOCTEST_NOEXCEPT

#ifndef DOCTEST_CONSTEXPR
#if DOCTEST_MSVC && (DOCTEST_MSVC < DOCTEST_COMPILER(19, 0, 0))
#define DOCTEST_CONSTEXPR const
#define DOCTEST_CONSTEXPR_FUNC inline
#else // DOCTEST_MSVC
#define DOCTEST_CONSTEXPR constexpr
#define DOCTEST_CONSTEXPR_FUNC constexpr
#endif // DOCTEST_MSVC
#endif // DOCTEST_CONSTEXPR

#ifndef DOCTEST_NO_SANITIZE_INTEGER
#if DOCTEST_CLANG >= DOCTEST_COMPILER(3, 7, 0)
#define DOCTEST_NO_SANITIZE_INTEGER __attribute__((no_sanitize("integer")))
#else
#define DOCTEST_NO_SANITIZE_INTEGER
#endif
#endif // DOCTEST_NO_SANITIZE_INTEGER

// this is kept here for backwards compatibility since the config option was changed
#ifdef DOCTEST_CONFIG_USE_IOSFWD
#ifndef DOCTEST_CONFIG_USE_STD_HEADERS
#define DOCTEST_CONFIG_USE_STD_HEADERS
#endif
#endif // DOCTEST_CONFIG_USE_IOSFWD

// for clang - always include <version> or <ciso646> (which drags some std stuff)
// because we want to check if we are using libc++ with the _LIBCPP_VERSION macro in
// which case we don't want to forward declare stuff from std - for reference:
// https://github.com/doctest/doctest/issues/126
// https://github.com/doctest/doctest/issues/356
#if DOCTEST_CLANG
#if DOCTEST_CPLUSPLUS >= 201703L && __has_include(<version>)
#include <version>
#else
#include <ciso646>
#endif
#endif // clang

#ifdef _LIBCPP_VERSION
#ifndef DOCTEST_CONFIG_USE_STD_HEADERS
#define DOCTEST_CONFIG_USE_STD_HEADERS
#endif
#endif // _LIBCPP_VERSION

#ifdef DOCTEST_CONFIG_USE_STD_HEADERS
#ifndef DOCTEST_CONFIG_INCLUDE_TYPE_TRAITS
#define DOCTEST_CONFIG_INCLUDE_TYPE_TRAITS
#endif // DOCTEST_CONFIG_INCLUDE_TYPE_TRAITS
#endif // DOCTEST_CONFIG_USE_STD_HEADERS

#if defined(__has_builtin)
#define DOCTEST_HAS_BUILTIN(x) __has_builtin(x)
#else
#define DOCTEST_HAS_BUILTIN(x) 0
#endif // __has_builtin

#endif // DOCTEST_PARTS_PUBLIC_CONFIG

// =================================================================================================
// == FEATURE DETECTION END ========================================================================
// =================================================================================================
#ifndef DOCTEST_PARTS_PUBLIC_UTILITY
#define DOCTEST_PARTS_PUBLIC_UTILITY


DOCTEST_SUPPRESS_PUBLIC_WARNINGS_PUSH

#define DOCTEST_DECLARE_INTERFACE(name)                                                                                \
    virtual ~name();                                                                                                   \
    name() = default;                                                                                                  \
    name(const name &) = delete;                                                                                       \
    name(name &&) = delete;                                                                                            \
    name &operator=(const name &) = delete;                                                                            \
    name &operator=(name &&) = delete;

#define DOCTEST_DEFINE_INTERFACE(name) name::~name() = default;

#if !defined(DOCTEST_COUNTER)
#if DOCTEST_CLANG >= DOCTEST_COMPILER(22, 0, 0)
#define DOCTEST_COUNTER __LINE__
#elif defined(__COUNTER__)
#define DOCTEST_COUNTER __COUNTER__
#else
#define DOCTEST_COUNTER __LINE__
#endif
#endif // defined(DOCTEST_COUNTER)

// internal macros for string concatenation and anonymous variable name generation
#define DOCTEST_CAT_IMPL(s1, s2) s1##s2
#define DOCTEST_CAT(s1, s2) DOCTEST_CAT_IMPL(s1, s2)
#define DOCTEST_ANONYMOUS(x) DOCTEST_CAT(x, DOCTEST_COUNTER)

#ifndef DOCTEST_CONFIG_ASSERTION_PARAMETERS_BY_VALUE
#define DOCTEST_REF_WRAP(x) x &
#else // DOCTEST_CONFIG_ASSERTION_PARAMETERS_BY_VALUE
#define DOCTEST_REF_WRAP(x) x
#endif // DOCTEST_CONFIG_ASSERTION_PARAMETERS_BY_VALUE

namespace doctest {
namespace detail {
DOCTEST_CLANG_SUPPRESS_WARNING_WITH_PUSH("-Wunused-function")
static DOCTEST_CONSTEXPR int consume(const int *, int) noexcept {
    return 0;
}
DOCTEST_CLANG_SUPPRESS_WARNING_POP
} // namespace detail
} // namespace doctest

#define DOCTEST_GLOBAL_NO_WARNINGS(var, ...)                                                                           \
    DOCTEST_CLANG_SUPPRESS_WARNING_WITH_PUSH("-Wglobal-constructors")                                                  \
    static const int var = doctest::detail::consume(&var, __VA_ARGS__);                                                \
    DOCTEST_CLANG_SUPPRESS_WARNING_POP

DOCTEST_SUPPRESS_PUBLIC_WARNINGS_POP

#endif // DOCTEST_PARTS_PUBLIC_UTI