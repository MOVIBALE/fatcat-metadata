#pragma once

#include <filesystem>

namespace fatcat::detail {

// Use the extended Windows namespace at the I/O boundary, independently of
// the host executable's manifest and the machine's LongPathsEnabled setting.
// Keep filesystem canonicalization separate: it must still resolve junctions
// and symlinks before callers perform containment checks.
inline std::filesystem::path io_path(const std::filesystem::path &path) {
#ifdef _WIN32
    auto absolute = std::filesystem::absolute(path).lexically_normal();
    absolute.make_preferred();
    const auto &name = absolute.native();
    if (name.rfind(L"\\\\?\\", 0) == 0) return absolute;
    if (name.rfind(L"\\\\", 0) == 0) {
        return std::filesystem::path(L"\\\\?\\UNC\\" + name.substr(2));
    }
    return std::filesystem::path(L"\\\\?\\" + name);
#else
    return path;
#endif
}

inline std::filesystem::path canonical_io_path(const std::filesystem::path &path) {
    // MSVC's weakly_canonical may remove the extended prefix. Reapply it both
    // for consistent root comparisons and for the subsequent status/open call.
    return io_path(std::filesystem::weakly_canonical(io_path(path)));
}

}  // namespace fatcat::detail
