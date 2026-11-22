#include <cstring>
#include <vector>

#include "package/xvd.h"
#include "test.h"

using namespace xbem;

namespace {
template <class T>
void put(std::vector<uint8_t>& b, size_t off, T v) {
  std::memcpy(b.data() + off, &v, sizeof v);
}
}  // namespace

TEST(xvd_header_parse) {
  std::vector<uint8_t> h(0x1000, 0);
  std::memcpy(h.data() + 0x200, "msft-xvd", 8);
  put<uint32_t>(h, 0x208, package::XvdInfo::kFlagReadOnly | package::XvdInfo::kFlagEncryptionDisabled);
  put<uint32_t>(h, 0x20C, 2);
  put<uint64_t>(h, 0x218, 0x40000000ull);
  put<uint32_t>(h, 0x280, 1);
  put<uint32_t>(h, 0x284, 1);  // Title
  put<uint32_t>(h, 0x290, 0x1000);

  const auto info = package::parse_xvd_header(h);
  CHECK(info.has_value());
  if (!info) return;
  CHECK_EQ(info->format_version, 2u);
  CHECK_EQ(info->drive_size, 0x40000000ull);
  CHECK(!info->encrypted());
  CHECK(info->is_xvc());
  CHECK_EQ(package::xvd_content_type_name(info->content_type), std::string("Title"));
  CHECK_EQ(package::xvd_flags_to_string(info->volume_flags), std::string("ReadOnly | EncryptionDisabled"));
}

TEST(xvd_rejects_non_xvd) {
  std::vector<uint8_t> h(0x1000, 0);
  CHECK(!package::parse_xvd_header(h).has_value());
  std::vector<uint8_t> tiny(16, 0);
  CHECK(!package::parse_xvd_header(tiny).has_value());
}

TEST(xvd_encrypted_by_default) {
  package::XvdInfo info;
  CHECK(info.encrypted());
  CHECK_EQ(package::xvd_content_type_name(0x1234), std::string("Unknown(0x1234)"));
}
