#pragma once

#define ARIA_VERSION_MAJOR 1
#define ARIA_VERSION_MINOR 0
#define ARIA_VERSION_PATCH 0

#define ARIA_VERSION_STRING "1.0.0"

#define ARIA_ABI_VERSION 1

namespace aria::abi {

constexpr int version_major = ARIA_VERSION_MAJOR;
constexpr int version_minor = ARIA_VERSION_MINOR;
constexpr int version_patch = ARIA_VERSION_PATCH;
constexpr int abi_version = ARIA_ABI_VERSION;
constexpr const char* version_string = ARIA_VERSION_STRING;

}  // namespace aria::abi
