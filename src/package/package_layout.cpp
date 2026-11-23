#include "package/package_layout.h"

#include <algorithm>
#include <stdexcept>

#include "common/file.h"
#include "common/strings.h"

namespace xbem::package {

namespace fs = std::filesystem;

PackageLayout scan_package(const fs::path& target) {
  std::error_code ec;
  if (!fs::exists(target, ec)) throw std::runtime_error("path does not exist: " + target.string());

  PackageLayout layout;
  fs::path explicit_exe;
  if (fs::is_regular_file(target, ec)) {
    explicit_exe = fs::absolute(target);
    layout.root = explicit_exe.parent_path();
  } else {
    layout.root = fs::absolute(target);
  }

  if (auto cfg = find_icase(layout.root, "MicrosoftGame.config")) {
    layout.config_path = *cfg;
    try {
      layout.config = load_game_config(*cfg);
    } catch (const std::exception& e) {
      layout.config_error = e.what();
    }
  }

  const auto opts = fs::directory_options::skip_permission_denied;
  for (fs::recursive_directory_iterator it(layout.root, opts, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    ++layout.file_count;
    layout.total_bytes += it->file_size(ec);
    const std::string ext = to_lower(it->path().extension().string());
    const fs::path rel = fs::relative(it->path(), layout.root, ec);
    if (ext == ".exe" || ext == ".dll") layout.pe_files.push_back(rel);
    else if (ext == ".xvc" || ext == ".msixvc" || ext == ".xvd") layout.container_files.push_back(rel);
  }
  std::sort(layout.pe_files.begin(), layout.pe_files.end());
  std::sort(layout.container_files.begin(), layout.container_files.end());

  if (!explicit_exe.empty()) {
    layout.main_executable = explicit_exe;
  } else if (layout.config) {
    if (const GameExecutable* e = layout.config->pick_executable()) {
      fs::path candidate = layout.root;
      // Executable names may contain sub-directories with backslashes.
      for (std::string_view part : split(e->name, '\\')) {
        if (part.empty()) continue;
        auto found = find_icase(candidate, part);
        candidate = found ? *found : candidate / std::string(part);
      }
      if (fs::exists(candidate, ec)) layout.main_executable = candidate;
      else layout.missing_executable = e->name;
    }
  }
  if (layout.main_executable.empty() && layout.missing_executable.empty()) {
    // Fall back to the only .exe at the top level, if unambiguous.
    std::vector<fs::path> top;
    for (const fs::path& p : layout.pe_files)
      if (!p.has_parent_path() && iequals(p.extension().string(), ".exe")) top.push_back(p);
    if (top.size() == 1) layout.main_executable = layout.root / top.front();
  }
  return layout;
}

}  // namespace xbem::package
