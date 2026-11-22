#include "package/xvd.h"

#include <cstring>
#include <format>

namespace xbem::package {

namespace {
template <class T>
T rd(std::span<const uint8_t> d, size_t off) {
  T v;
  std::memcpy(&v, d.data() + off, sizeof(T));
  return v;
}
}  // namespace

std::optional<XvdInfo> parse_xvd_header(std::span<const uint8_t> d) {
  if (d.size() < kXvdMinHeaderSize) return std::nullopt;
  if (std::memcmp(d.data() + kXvdMagicOffset, "msft-xvd", 8) != 0) return std::nullopt;
  XvdInfo x;
  x.volume_flags = rd<uint32_t>(d, 0x208);
  x.format_version = rd<uint32_t>(d, 0x20C);
  x.creation_filetime = rd<uint64_t>(d, 0x210);
  x.drive_size = rd<uint64_t>(d, 0x218);
  std::memcpy(x.vduid.data(), d.data() + 0x220, 16);
  std::memcpy(x.uduid.data(), d.data() + 0x230, 16);
  x.type = rd<uint32_t>(d, 0x280);
  x.content_type = rd<uint32_t>(d, 0x284);
  x.embedded_xvd_length = rd<uint32_t>(d, 0x288);
  x.user_data_length = rd<uint32_t>(d, 0x28C);
  x.xvc_data_length = rd<uint32_t>(d, 0x290);
  x.dynamic_header_length = rd<uint32_t>(d, 0x294);
  x.block_size = rd<uint32_t>(d, 0x298);
  return x;
}

std::string xvd_content_type_name(uint32_t t) {
  static const char* const names[] = {
      "Data",          "Title",       "SystemOS",       "EraOS",          "Scratch",        "ResetData",
      "Application",   "HostOS",      "X360STFS",       "X360FATX",       "X360GDFX",       "Updater",
      "OfflineUpdater", "Template",   "MteHost",        "MteApp",         "MteTitle",       "MteEraOS",
      "EraTools",      "SystemTools", "SystemAux",      "AcousticModel",  "SystemCodecsVolume", "QasltPackage",
      "AppDlc",        "TitleDlc",    "UniversalDlc",
  };
  if (t < sizeof(names) / sizeof(names[0])) return names[t];
  return std::format("Unknown(0x{:x})", t);
}

std::string xvd_flags_to_string(uint32_t f) {
  static const struct {
    uint32_t bit;
    const char* name;
  } kFlags[] = {{0x1, "ReadOnly"},          {0x2, "EncryptionDisabled"}, {0x4, "DataIntegrityDisabled"},
                {0x8, "LegacySectorSize"},  {0x10, "ResiliencyEnabled"}, {0x20, "SraReadOnly"},
                {0x40, "RegionIdInXts"},    {0x80, "EraSpecific"}};
  std::string out;
  for (const auto& fl : kFlags) {
    if (!(f & fl.bit)) continue;
    if (!out.empty()) out += " | ";
    out += fl.name;
  }
  const uint32_t unknown = f & ~0xFFu;
  if (unknown) out += std::format("{}0x{:x}", out.empty() ? "" : " | ", unknown);
  return out.empty() ? "none" : out;
}

}  // namespace xbem::package
