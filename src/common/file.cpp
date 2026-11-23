#include "common/file.h"

#include <fstream>
#include <stdexcept>

#include "common/strings.h"

namespace xbem {

std::vector<uint8_t> read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) throw std::runtime_error("cannot open file: " + path.string());
  const std::streamoff size = in.tellg();
  if (size < 0) throw std::runtime_error("cannot determine size of: " + path.string());
  std::vector<uint8_t> data(static_cast<size_t>(size));
  in.seekg(0);
  if (size > 0 && !in.read(reinterpret_cast<char*>(data.data()), size))
    throw std::runtime_error("cannot read file: " + path.string());
  return data;
}

std::vector<uint8_t> read_file_prefix(const std::filesystem::path& path, size_t max_bytes) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open file: " + path.string());
  std::vector<uint8_t> data(max_bytes);
  in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(max_bytes));
  data.resize(static_cast<size_t>(in.gcount()));
  return data;
}

std::optional<std::filesystem::path> find_icase(const std::filesystem::path& dir, std::string_view name) {
  std::error_code ec;
#if !defined(_WIN32) && !defined(__APPLE__)
  // On a case-sensitive file system an exact hit is already the on-disk name.
  const auto exact = dir / std::filesystem::path(std::string(name));
  if (std::filesystem::exists(exact, ec)) return exact;
#endif
  // Otherwise scan the directory so the result carries the real on-disk
  // spelling (case-insensitive file systems would accept any spelling).
  std::optional<std::filesystem::path> folded;
  for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string entry = it->path().filename().string();
    if (entry == name) return it->path();
    if (!folded && iequals(entry, name)) folded = it->path();
  }
  return folded;
}

}  // namespace xbem
