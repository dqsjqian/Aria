#pragma once

#include <filesystem>

namespace aria::adapters::http::detail {

// Both paths are already canonical. A volume root is a component, not a
// string prefix to which another directory separator may be appended.
inline bool within_static_mount(const std::filesystem::path& root,
                                const std::filesystem::path& resolved) {
    const auto relative = resolved.lexically_relative(root);
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}

}  // namespace aria::adapters::http::detail
