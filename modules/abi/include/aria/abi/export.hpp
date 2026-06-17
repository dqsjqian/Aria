#pragma once

// Cross-platform symbol visibility macros.
// Each shared library defines ARIA_<MODULE>_BUILD when compiling its own sources.

#if defined(_WIN32) || defined(__CYGWIN__)
#  define ARIA_EXPORT __declspec(dllexport)
#  define ARIA_IMPORT __declspec(dllimport)
#else
#  define ARIA_EXPORT __attribute__((visibility("default")))
#  define ARIA_IMPORT __attribute__((visibility("default")))
#endif

// ABI module
#if defined(ARIA_ABI_BUILD)
#  define ARIA_ABI_API ARIA_EXPORT
#elif defined(ARIA_ABI_STATIC)
// ARIA_ABI_STATIC is only defined on Windows (see modules/abi/CMakeLists.txt).
// It prevents dllimport/dllexport on abi symbols when abi is a static library.
#  define ARIA_ABI_API
#else
#  define ARIA_ABI_API ARIA_IMPORT
#endif

// Runtime module
#if defined(ARIA_RUNTIME_BUILD)
#  define ARIA_RUNTIME_API ARIA_EXPORT
#elif defined(ARIA_RUNTIME_STATIC)
#  define ARIA_RUNTIME_API
#else
#  define ARIA_RUNTIME_API ARIA_IMPORT
#endif

// Binding module
#if defined(ARIA_BINDING_BUILD)
#  define ARIA_BINDING_API ARIA_EXPORT
#elif defined(ARIA_BINDING_STATIC)
#  define ARIA_BINDING_API
#else
#  define ARIA_BINDING_API ARIA_IMPORT
#endif

// Core module (diagnostics/trace sink storage lives in runtime DLL)
#if defined(ARIA_RUNTIME_BUILD)
#  define ARIA_CORE_API ARIA_EXPORT
#elif defined(ARIA_RUNTIME_STATIC)
#  define ARIA_CORE_API
#else
#  define ARIA_CORE_API ARIA_IMPORT
#endif

// Qt6 adapter module
#if defined(ARIA_QT6_BUILD)
#  define ARIA_QT6_API ARIA_EXPORT
#elif defined(ARIA_QT6_STATIC)
#  define ARIA_QT6_API
#else
#  define ARIA_QT6_API ARIA_IMPORT
#endif

// HTTP adapter module
#if defined(ARIA_HTTP_BUILD)
#  define ARIA_HTTP_API ARIA_EXPORT
#elif defined(ARIA_HTTP_STATIC)
#  define ARIA_HTTP_API
#else
#  define ARIA_HTTP_API ARIA_IMPORT
#endif

// JNI (Android) adapter module
#if defined(ARIA_JNI_BUILD)
#  define ARIA_JNI_API ARIA_EXPORT
#elif defined(ARIA_JNI_STATIC)
#  define ARIA_JNI_API
#else
#  define ARIA_JNI_API ARIA_IMPORT
#endif

// Force-inline
#if defined(_MSC_VER)
#  define ARIA_ALWAYS_INLINE __forceinline
#else
#  define ARIA_ALWAYS_INLINE __attribute__((always_inline)) inline
#endif

// Branch-prediction hints.
//
// `__builtin_expect` is a GCC/Clang extension that MSVC does not provide.
// We therefore use it only on compilers that support it, and degrade to a
// plain expression elsewhere. The C++20 [[likely]] / [[unlikely]] attributes
// cannot be used here because these macros are expanded inside ordinary
// expressions (e.g. `if (ARIA_LIKELY(ptr)) ...`), not on statements.
#if defined(__GNUC__) || defined(__clang__)
#  define ARIA_LIKELY(x)   (__builtin_expect(!!(x), 1))
#  define ARIA_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#else
#  define ARIA_LIKELY(x)   (!!(x))
#  define ARIA_UNLIKELY(x) (!!(x))
#endif
