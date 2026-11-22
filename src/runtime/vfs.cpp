#include "runtime/vfs.h"

#include <vector>

#include "common/file.h"
#include "common/strings.h"

namespace xbem::rt {

namespace fs = std::filesystem;

void Vfs::mount(char drive, fs::path host_root, bool read_only) {
  mounts_[upper(drive)] = Mount{fs::absolute(std::move(host_root)).lexically_normal(), read_only};
}

std::optional<Vfs::Resolved> Vfs::resolve(std::u16string_view guest_path) const {
  return resolve(std::string_view(utf16_to_utf8(guest_path)));
}

std::optional<Vfs::Resolved> Vfs::resolve(std::string_view input) const {
  std::string p(input);
  for (char& c : p)
    if (c == '\\') c = '/';
  // Win32 namespace prefixes: \\?\C:\x and \\.\C:\x
  if (p.rfind("//?/", 0) == 0 || p.rfind("//./", 0) == 0) p = p.substr(4);

  char drive = default_drive_;
  std::string_view rest = p;
  if (rest.size() >= 2 && rest[1] == ':' &&
      ((rest[0] >= 'a' && rest[0] <= 'z') || (rest[0] >= 'A' && rest[0] <= 'Z'))) {
    drive = upper(rest[0]);
    rest = rest.substr(2);
  }
  auto mit = mounts_.find(drive);
  if (mit == mounts_.end()) return std::nullopt;

  // Lexical normalisation that clamps ".." at the mount root.
  std::vector<std::string_view> parts;
  for (std::string_view part : split(rest, '/')) {
    if (part.empty() || part == ".") continue;
    if (part == "..") {
      if (!parts.empty()) parts.pop_back();
      continue;
    }
    parts.push_back(part);
  }

  fs::path current = mit->second.host_root;
  bool exists_so_far = true;
  for (std::string_view part : parts) {
    if (exists_so_far) {
      if (auto found = find_icase(current, part)) {
        current = *found;
        continue;
      }
      exists_so_far = false;
    }
    current /= std::string(part);
  }
  return Resolved{current, mit->second.read_only};
}

std::optional<std::string> Vfs::to_guest(const fs::path& host_path) const {
  const fs::path abs = fs::absolute(host_path).lexically_normal();
  for (const auto& [drive, m] : mounts_) {
    const fs::path rel = abs.lexically_relative(m.host_root);
    if (rel.empty() || *rel.begin() == "..") continue;
    std::string out = std::string(1, drive) + ":\\";
    std::string r = rel.generic_string();
    if (r == ".") r.clear();
    for (char& c : r)
      if (c == '/') c = '\\';
    return out + r;
  }
  return std::nullopt;
}

}  // namespace xbem::rt
