#include "common/strings.h"
#include "test.h"

using namespace xbem;

TEST(utf8_utf16_roundtrip) {
  const std::string s = "Ahoj \xC4\x8D" "e\xC5\xA1tina \xF0\x9F\x98\x80";  // "Ahoj čeština 😀"
  const std::u16string w = utf8_to_utf16(s);
  CHECK_EQ(w.size(), 15u);  // emoji = surrogate pair
  CHECK(w[5] == u'č');
  CHECK(w[13] == 0xD83D && w[14] == 0xDE00);
  CHECK_EQ(utf16_to_utf8(w), s);
}

TEST(utf16_unpaired_surrogate_is_replaced) {
  const std::u16string w = {u'a', static_cast<char16_t>(0xD800), u'b'};
  CHECK_EQ(utf16_to_utf8(w), std::string("a\xEF\xBF\xBD" "b"));
}

TEST(case_insensitive_helpers) {
  CHECK(iequals("KernelX.DLL", "kernelx.dll"));
  CHECK(!iequals("kernelx.dll", "kernelx.dl"));
  CHECK(istarts_with("API-MS-WIN-core", "api-ms-win-"));
  CHECK(iends_with("Game.EXE", ".exe"));
  CHECK_EQ(to_lower("MiXeD"), std::string("mixed"));
}

TEST(wildcards) {
  CHECK(wildcard_match_icase("*.txt", "Data.TXT"));
  CHECK(!wildcard_match_icase("*.txt", "data.bin"));
  CHECK(wildcard_match_icase("*", "anything"));
  CHECK(wildcard_match_icase("*.*", "README"));
  CHECK(wildcard_match_icase("level??.pak", "LEVEL01.PAK"));
  CHECK(!wildcard_match_icase("level??.pak", "level1.pak"));
  CHECK(wildcard_match_icase("a*b*c", "aXXbYYc"));
}

TEST(split_and_format) {
  const auto parts = split("a\\b\\\\c", '\\');
  CHECK_EQ(parts.size(), 4u);
  CHECK(parts[0] == "a" && parts[2].empty() && parts[3] == "c");
  CHECK_EQ(format_bytes(512), std::string("512 B"));
  CHECK_EQ(format_bytes(1536), std::string("1.50 KiB"));
  const uint8_t guid[16] = {0x78, 0x56, 0x34, 0x12, 0x34, 0x12, 0x78, 0x56, 1, 2, 3, 4, 5, 6, 7, 8};
  CHECK_EQ(guid_to_string(guid), std::string("12345678-1234-5678-0102-030405060708"));
}
