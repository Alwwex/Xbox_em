#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace xbem::rt {

// Translates guest (Windows-style) paths to host paths.
//
//  * Drive letters are mapped to host directories ("mounts"). By default the
//    game package is mounted read-only (G:) and a writable scratch directory is
//    mounted for temporary data (T:). Mounts are configurable (--mount).
//  * Relative paths and rooted paths without a drive ("\data\x.bin") resolve
//    against the package root, which is the working directory of a title.
//  * Windows paths are case-insensitive; on case-sensitive host file systems
//    each existing path component is matched case-insensitively.
//  * ".." can never escape the mount root.
class Vfs {
 public:
  struct Mount {
    std::filesystem::path host_root;
    bool read_only = false;
  };

  struct Resolved {
    std::filesystem::path host_path;
    bool read_only = false;
  };

  void mount(char drive, std::filesystem::path host_root, bool read_only);
  void set_default_drive(char drive) { default_drive_ = upper(drive); }
  char default_drive() const { return default_drive_; }
  const std::map<char, Mount>& mounts() const { return mounts_; }

  std::optional<Resolved> resolve(std::string_view guest_path) const;
  std::optional<Resolved> resolve(std::u16string_view guest_path) const;

  // Inverse mapping for things like GetModuleFileName: host path -> "G:\\dir\\file".
  std::optional<std::string> to_guest(const std::filesystem::path& host_path) const;

 private:
  static char upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }
  std::map<char, Mount> mounts_;
  char default_drive_ = 'G';
};

}  // namespace xbem::rt
