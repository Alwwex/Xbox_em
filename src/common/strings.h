#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace xbem {

std::string to_lower(std::string_view s);
bool iequals(std::string_view a, std::string_view b);
bool istarts_with(std::string_view s, std::string_view prefix);
bool iends_with(std::string_view s, std::string_view suffix);

std::string utf16_to_utf8(std::u16string_view s);
std::u16string utf8_to_utf16(std::string_view s);

// Length of a NUL-terminated guest string, bounded by `max`.
size_t bounded_strlen(const char* s, size_t max = 1u << 20);
size_t bounded_strlen16(const char16_t* s, size_t max = 1u << 20);

inline std::string_view guest_cstr(const char* s) {
  return s ? std::string_view(s, bounded_strlen(s)) : std::string_view();
}
inline std::u16string_view guest_wstr(const char16_t* s) {
  return s ? std::u16string_view(s, bounded_strlen16(s)) : std::u16string_view();
}

// Glob match with '*' and '?', ASCII case-insensitive (Windows FindFirstFile semantics, simplified).
bool wildcard_match_icase(std::string_view pattern, std::string_view text);

std::string format_bytes(uint64_t bytes);
std::string guid_to_string(const uint8_t bytes[16]);

std::vector<std::string_view> split(std::string_view s, char sep);

}  // namespace xbem
