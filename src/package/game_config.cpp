#include "package/game_config.h"

#include <stdexcept>

#include "common/file.h"
#include "common/strings.h"
#include "common/xml.h"

namespace xbem::package {

const GameExecutable* GameConfig::pick_executable(std::string_view family) const {
  for (const auto& e : executables)
    if (iequals(e.target_device_family, family)) return &e;
  for (const auto& e : executables)
    if (!iequals(e.target_device_family, "PC")) return &e;
  return executables.empty() ? nullptr : &executables.front();
}

GameConfig parse_game_config(std::string_view xml_text) {
  const xml::Node root = xml::parse(xml_text);
  if (!iequals(root.name, "Game"))
    throw std::runtime_error("MicrosoftGame.config: root element is <" + root.name + ">, expected <Game>");

  GameConfig cfg;
  cfg.config_version = root.attr_or("configVersion");
  if (const xml::Node* id = root.child("Identity")) {
    cfg.identity_name = id->attr_or("Name");
    cfg.publisher = id->attr_or("Publisher");
    cfg.version = id->attr_or("Version");
  }
  cfg.title_id = root.child_text("TitleId");
  cfg.store_id = root.child_text("StoreId");
  cfg.msa_app_id = root.child_text("MSAAppId");
  if (const xml::Node* sv = root.child("ShellVisuals")) {
    cfg.display_name = sv->attr_or("DefaultDisplayName");
    cfg.publisher_display_name = sv->attr_or("PublisherDisplayName");
    cfg.description = sv->attr_or("Description");
  }
  if (const xml::Node* list = root.child("ExecutableList")) {
    for (const xml::Node* e : list->children_named("Executable")) {
      GameExecutable ge;
      ge.name = e->attr_or("Name");
      ge.target_device_family = e->attr_or("TargetDeviceFamily");
      ge.id = e->attr_or("Id");
      if (!ge.name.empty()) cfg.executables.push_back(std::move(ge));
    }
  }
  return cfg;
}

GameConfig load_game_config(const std::filesystem::path& path) {
  const std::vector<uint8_t> data = read_file(path);
  return parse_game_config(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
}

}  // namespace xbem::package
