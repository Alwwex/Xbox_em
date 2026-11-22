#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace xbem::package {

// One <Executable> entry from MicrosoftGame.config.
struct GameExecutable {
  std::string name;                  // relative path inside the package, e.g. "Game.exe"
  std::string target_device_family;  // "Scarlett" (Xbox Series), "XboxOne" or "PC"
  std::string id;
};

// The subset of MicrosoftGame.config (GDK game manifest) that the emulator needs.
struct GameConfig {
  std::string config_version;
  std::string identity_name;
  std::string publisher;
  std::string version;
  std::string title_id;
  std::string store_id;
  std::string msa_app_id;
  std::string display_name;
  std::string publisher_display_name;
  std::string description;
  std::vector<GameExecutable> executables;

  // Prefers an executable built for `family` ("Scarlett" by default), then any console one.
  const GameExecutable* pick_executable(std::string_view family = "Scarlett") const;
};

GameConfig parse_game_config(std::string_view xml_text);
GameConfig load_game_config(const std::filesystem::path& path);

}  // namespace xbem::package
