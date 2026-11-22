#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace xbem {

// Reads a whole file. Throws std::runtime_error on failure.
std::vector<uint8_t> read_file(const std::filesystem::path& path);

// Reads at most `max_bytes` from the beginning of a file.
std::vector<uint8_t> read_file_prefix(const std::filesystem::path& path, size_t max_bytes);

// Finds `name` inside `dir` using ASCII case-insensitive comparison
// (Windows file systems are case-insensitive, Linux ones usually are not).
std::optional<std::filesystem::path> find_icase(const std::filesystem::path& dir, std::string_view name);

}  // namespace xbem
