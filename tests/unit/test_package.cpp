#include <filesystem>
#include <fstream>

#include "package/game_config.h"
#include "package/package_layout.h"
#include "test.h"

using namespace xbem;
namespace fs = std::filesystem;

namespace {

const char* kConfig = R"(<?xml version="1.0" encoding="utf-8"?>
<Game configVersion="1">
  <Identity Name="Contoso.Game" Publisher="CN=Contoso" Version="1.2.3.0"/>
  <ExecutableList>
    <Executable Name="PcGame.exe" TargetDeviceFamily="PC" Id="Game"/>
    <Executable Name="Bin\ConsoleGame.exe" TargetDeviceFamily="Scarlett" Id="Game"/>
  </ExecutableList>
  <ShellVisuals DefaultDisplayName="Contoso Game" PublisherDisplayName="Contoso" Description="A game"/>
  <TitleId>12345678</TitleId>
  <StoreId>9ABCDEF</StoreId>
  <MSAAppId>0000000040000000</MSAAppId>
</Game>)";

fs::path make_temp_dir(const char* name) {
  const fs::path p = fs::temp_directory_path() / "xbem-unit" / name;
  fs::remove_all(p);
  fs::create_directories(p);
  return p;
}

void touch(const fs::path& p, const std::string& content = "x") {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << content;
}

}  // namespace

TEST(game_config_parse) {
  const package::GameConfig c = package::parse_game_config(kConfig);
  CHECK_EQ(c.identity_name, std::string("Contoso.Game"));
  CHECK_EQ(c.version, std::string("1.2.3.0"));
  CHECK_EQ(c.title_id, std::string("12345678"));
  CHECK_EQ(c.store_id, std::string("9ABCDEF"));
  CHECK_EQ(c.display_name, std::string("Contoso Game"));
  CHECK_EQ(c.executables.size(), 2u);
  const auto* e = c.pick_executable();
  CHECK(e != nullptr);
  if (e) CHECK_EQ(e->name, std::string("Bin\\ConsoleGame.exe"));
  CHECK_EQ(c.pick_executable("PC")->name, std::string("PcGame.exe"));
}

TEST(game_config_rejects_other_roots) { CHECK_THROWS(package::parse_game_config("<Package/>")); }

TEST(package_scan_uses_config_case_insensitively) {
  const fs::path root = make_temp_dir("pkg1");
  touch(root / "microsoftgame.config", kConfig);
  touch(root / "bin" / "consolegame.EXE");
  touch(root / "bin" / "engine.dll");
  touch(root / "PcGame.exe");
  touch(root / "data" / "level.pak", std::string(1000, 'x'));
  touch(root / "update.msixvc");

  const auto layout = package::scan_package(root);
  CHECK(layout.config.has_value());
  CHECK(layout.config_error.empty());
  CHECK_EQ(layout.main_executable.filename().string(), std::string("consolegame.EXE"));
  CHECK_EQ(layout.pe_files.size(), 3u);
  CHECK_EQ(layout.container_files.size(), 1u);
  CHECK_EQ(layout.file_count, 6u);
}

TEST(package_scan_fallback_single_exe) {
  const fs::path root = make_temp_dir("pkg2");
  touch(root / "Game.exe");
  touch(root / "sub" / "Tool.exe");
  const auto layout = package::scan_package(root);
  CHECK(!layout.config.has_value());
  CHECK_EQ(layout.main_executable.filename().string(), std::string("Game.exe"));

  const auto direct = package::scan_package(root / "sub" / "Tool.exe");
  CHECK_EQ(direct.main_executable.filename().string(), std::string("Tool.exe"));
}

TEST(package_scan_reports_broken_config) {
  const fs::path root = make_temp_dir("pkg3");
  touch(root / "MicrosoftGame.config", "<Game><Identity></Game>");
  const auto layout = package::scan_package(root);
  CHECK(!layout.config.has_value());
  CHECK(!layout.config_error.empty());
}

TEST(package_scan_reports_missing_executable) {
  // MicrosoftGame.config names an exe that is not in the folder (incomplete build).
  const fs::path root = make_temp_dir("pkg4");
  touch(root / "MicrosoftGame.config", kConfig);
  touch(root / "helper.dll");
  touch(root / "gameos.xvd");
  const auto layout = package::scan_package(root);
  CHECK(layout.main_executable.empty());
  CHECK_EQ(layout.missing_executable, std::string("Bin\\ConsoleGame.exe"));
  CHECK_EQ(layout.container_files.size(), 1u);
}
