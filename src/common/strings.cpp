#include "common/strings.h"

#include <cstdio>
#include <format>

namespace xbem {

namespace {
char lower_ascii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
}  // namespace

std::string to_lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = lower_ascii(c);
  return out;
}

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (lower_ascii(a[i]) != lower_ascii(b[i])) return false;
  return true;
}

bool istarts_with(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix);
}

bool iends_with(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && iequals(s.substr(s.size() - suffix.size()), suffix);
}

std::string utf16_to_utf8(std::u16string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    uint32_t cp = s[i];
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
      ++i;
    } else if (cp >= 0xD800 && cp <= 0xDFFF) {
      cp = 0xFFFD;  // unpaired surrogate
    }
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  return out;
}

std::u16string utf8_to_utf16(std::string_view s) {
  std::u16string out;
  out.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    const auto b0 = static_cast<uint8_t>(s[i]);
    uint32_t cp = 0xFFFD;
    size_t len = 1;
    auto cont = [&](size_t k) { return i + k < s.size() && (static_cast<uint8_t>(s[i + k]) & 0xC0) == 0x80; };
    auto cb = [&](size_t k) { return static_cast<uint32_t>(static_cast<uint8_t>(s[i + k]) & 0x3F); };
    if (b0 < 0x80) {
      cp = b0;
    } else if ((b0 & 0xE0) == 0xC0 && cont(1)) {
      cp = ((b0 & 0x1Fu) << 6) | cb(1);
      len = 2;
    } else if ((b0 & 0xF0) == 0xE0 && cont(1) && cont(2)) {
      cp = ((b0 & 0x0Fu) << 12) | (cb(1) << 6) | cb(2);
      len = 3;
    } else if ((b0 & 0xF8) == 0xF0 && cont(1) && cont(2) && cont(3)) {
      cp = ((b0 & 0x07u) << 18) | (cb(1) << 12) | (cb(2) << 6) | cb(3);
      len = 4;
    }
    i += len;
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
      out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
    } else {
      out.push_back(static_cast<char16_t>(cp));
    }
  }
  return out;
}

size_t bounded_strlen(const char* s, size_t max) {
  size_t n = 0;
  while (n < max && s[n] != 0) ++n;
  return n;
}

size_t bounded_strlen16(const char16_t* s, size_t max) {
  size_t n = 0;
  while (n < max && s[n] != 0) ++n;
  return n;
}

bool wildcard_match_icase(std::string_view p, std::string_view t) {
  size_t pi = 0, ti = 0, star = std::string_view::npos, mark = 0;
  while (ti < t.size()) {
    if (pi < p.size() && (p[pi] == '?' || lower_ascii(p[pi]) == lower_ascii(t[ti]))) {
      ++pi;
      ++ti;
    } else if (pi < p.size() && p[pi] == '*') {
      star = pi++;
      mark = ti;
    } else if (star != std::string_view::npos) {
      pi = star + 1;
      ti = ++mark;
    } else {
      return false;
    }
  }
  while (pi < p.size() && p[pi] == '*') ++pi;
  // "*.*" matches names without a dot on Windows.
  if (pi + 2 == p.size() && p.substr(pi) == ".*") return true;
  return pi == p.size();
}

std::string format_bytes(uint64_t bytes) {
  const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  double v = static_cast<double>(bytes);
  int u = 0;
  while (v >= 1024.0 && u < 4) {
    v /= 1024.0;
    ++u;
  }
  return u == 0 ? std::format("{} B", bytes) : std::format("{:.2f} {}", v, units[u]);
}

std::string guid_to_string(const uint8_t b[16]) {
  // Windows GUID layout: first three fields little-endian.
  return std::format("{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
                     b[3], b[2], b[1], b[0], b[5], b[4], b[7], b[6], b[8], b[9], b[10], b[11], b[12], b[13],
                     b[14], b[15]);
}

std::vector<std::string_view> split(std::string_view s, char sep) {
  std::vector<std::string_view> out;
  size_t start = 0;
  while (start <= s.size()) {
    const size_t pos = s.find(sep, start);
    if (pos == std::string_view::npos) {
      out.push_back(s.substr(start));
      break;
    }
    out.push_back(s.substr(start, pos - start));
    start = pos + 1;
  }
  return out;
}

}  // namespace xbem
