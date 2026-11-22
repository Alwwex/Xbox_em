#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "package/game_config.h"

namespace xbem::package {

// What we found in a "loose" game layout: the folder a GDK build produces
// before it is packaged (exe + DLLs + MicrosoftGame.config + content).
struct PackageLayout {
  std::filesystem::path root;
  std::optional<std::filesystem::path> config_path;
  std::optional<GameConfig> config;
  std::string config_error;               // set if MicrosoftGame.config exists but failed to parse
  std::filesystem::path main_executable;  // absolute; empty if it could not be determined
  std::vector<std::filesystem::path> pe_files;       // relative to root (.exe / .dll)
  std::vector<std::filesystem::path> container_files;  // .xvc / .msixvc / .xvd (packaged, usually encrypted)
  uint64_t total_bytes = 0;
  uint64_t file_count = 0;
};

// `target` may be a directory (loose layout) or a path to an .exe inside one.
PackageLayout scan_package(const std::filesystem::path& target);

}  // namespace xbem::package
