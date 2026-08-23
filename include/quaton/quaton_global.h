#ifndef QUATON_GLOBAL_H_
#define QUATON_GLOBAL_H_
#pragma once

// =============================================================================
// Platform Detection
// =============================================================================

#if defined(_WIN32) || defined(_WIN64)
#define QUATON_PLATFORM_WINDOWS
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_MAC
#define QUATON_PLATFORM_MACOS
#endif
#elif defined(__linux__)
#define QUATON_PLATFORM_LINUX
#else
#define QUATON_PLATFORM_UNKNOWN
#endif

// =============================================================================
// Export/Import Macros
// =============================================================================

// When built as a static library (QUATON_STATIC is defined by the build
// system) no __declspec is emitted: symbols are simply part of the static
// archive that the consuming target links against.
#ifdef QUATON_STATIC
#define QUATON_API
#elif defined(QUATON_PLATFORM_WINDOWS)
#ifdef QUATON_EXPORTS
#define QUATON_API __declspec(dllexport)
#else
#define QUATON_API __declspec(dllimport)
#endif
#else
#define QUATON_API
#endif

// =============================================================================
// Compiler Detection
// =============================================================================

#if defined(_MSC_VER)
#define QUATON_COMPILER_MSVC
#define QUATON_MSVC_VERSION _MSC_VER
#elif defined(__clang__)
#define QUATON_COMPILER_CLANG
#elif defined(__GNUC__)
#define QUATON_COMPILER_GCC
#endif

// =============================================================================
// C++ Standard Version
// =============================================================================

#if __cplusplus >= 202002L
#define QUATON_CPP20_OR_LATER
#elif __cplusplus >= 201703L
#define QUATON_CPP17_OR_LATER
#elif __cplusplus >= 201402L
#define QUATON_CPP14_OR_LATER
#endif

// =============================================================================
// Utility Macros
// =============================================================================

/// @brief Disables copy constructor and copy assignment operator
#define QUATON_DISABLE_COPY(Class) \
  Class(const Class&) = delete;    \
  Class& operator=(const Class&) = delete

/// @brief Disables move constructor and move assignment operator
#define QUATON_DISABLE_MOVE(Class) \
  Class(Class&&) = delete;         \
  Class& operator=(Class&&) = delete

/// @brief Disables both copy and move
#define QUATON_DISABLE_COPY_MOVE(Class) \
  QUATON_DISABLE_COPY(Class);           \
  QUATON_DISABLE_MOVE(Class)

/// @brief Default move operations
#define QUATON_DEFAULT_MOVE(Class)   \
  Class(Class&&) noexcept = default; \
  Class& operator=(Class&&) noexcept = default

// =============================================================================
// Namespace Macros
// =============================================================================

#define QUATON_NAMESPACE_BEGIN namespace Quaton {
#define QUATON_NAMESPACE_END }

#endif  // QUATON_GLOBAL_H_
