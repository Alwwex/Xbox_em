#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace xbem::package {

// Read-only view of an XVD/XVC container header (the format used for installed
// Xbox One / Series packages). Field layout follows public community research
// (e.g. the open-source "xvdtool" project).
//
// xbem deliberately does NOT decrypt containers and does not deal with keys:
// it only reports metadata so you can tell whether you were given an encrypted
// retail-style package (unusable) or need to ask for a loose/unencrypted build.
struct XvdInfo {
  uint32_t volume_flags = 0;
  uint32_t format_version = 0;
  uint64_t creation_filetime = 0;
  uint64_t drive_size = 0;
  std::array<uint8_t, 16> vduid{};
  std::array<uint8_t, 16> uduid{};
  uint32_t type = 0;  // 0 = fixed, 1 = dynamic
  uint32_t content_type = 0;
  uint32_t embedded_xvd_length = 0;
  uint32_t user_data_length = 0;
  uint32_t xvc_data_length = 0;
  uint32_t dynamic_header_length = 0;
  uint32_t block_size = 0;

  static constexpr uint32_t kFlagReadOnly = 0x1;
  static constexpr uint32_t kFlagEncryptionDisabled = 0x2;
  static constexpr uint32_t kFlagDataIntegrityDisabled = 0x4;

  bool encrypted() const { return !(volume_flags & kFlagEncryptionDisabled); }
  bool is_xvc() const { return xvc_data_length != 0; }
};

constexpr size_t kXvdMagicOffset = 0x200;
constexpr size_t kXvdMinHeaderSize = 0x29C;

// Returns nullopt if the data does not start with an XVD header ("msft-xvd" magic).
std::optional<XvdInfo> parse_xvd_header(std::span<const uint8_t> data);

std::string xvd_content_type_name(uint32_t content_type);
std::string xvd_flags_to_string(uint32_t flags);

}  // namespace xbem::package
