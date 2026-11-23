// kernelx: locale, NLS string functions, date/time formatting and
// FormatMessage. Titles run with the console's user locale; every locale is
// answered with en-US data (names, separators, calendars), which is what a
// console set to English (United States) reports.

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <utility>

#include "hle/heap.h"
#include <vector>
#include "hle/hle_util.h"

namespace xbem::hle {

void fill_local_systemtime(void* out);  // kernelx.cpp

namespace {

using namespace win;

constexpr DWORD ERROR_MR_MID_NOT_FOUND = 317;
constexpr DWORD ERROR_INVALID_FLAGS = 1004;

// ------------------------------------------------------------ helpers

std::u16string_view wview(LPCWSTR s, int len) {
  if (!s) return {};
  if (len < 0) return guest_wstr(s);
  return std::u16string_view(s, static_cast<size_t>(len));
}

char16_t fold_upper(char16_t c) {
  if (c >= u'a' && c <= u'z') return static_cast<char16_t>(c - 32);
  if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return static_cast<char16_t>(c - 32);
  return c;
}
char16_t fold_lower(char16_t c) {
  if (c >= u'A' && c <= u'Z') return static_cast<char16_t>(c + 32);
  if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return static_cast<char16_t>(c + 32);
  return c;
}

// Copies a wide result following the NLS convention: with cch == 0 return the
// required size (including NUL); otherwise copy and return characters written
// including NUL, or 0 with ERROR_INSUFFICIENT_BUFFER.
int nls_copy_out(std::u16string_view s, LPWSTR out, int cch) {
  const int need = static_cast<int>(s.size()) + 1;
  if (cch == 0) return need;
  if (!out || cch < need) {
    set_last_error(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  std::copy(s.begin(), s.end(), out);
  out[s.size()] = 0;
  return need;
}

// ------------------------------------------------------------ locale data

constexpr const char16_t* kDayNames[7] = {u"Monday", u"Tuesday", u"Wednesday", u"Thursday",
                                          u"Friday", u"Saturday", u"Sunday"};
constexpr const char16_t* kDayAbbrev[7] = {u"Mon", u"Tue", u"Wed", u"Thu", u"Fri", u"Sat", u"Sun"};
constexpr const char16_t* kMonthNames[12] = {u"January", u"February", u"March",     u"April",
                                             u"May",     u"June",     u"July",      u"August",
                                             u"September", u"October", u"November", u"December"};
constexpr const char16_t* kMonthAbbrev[12] = {u"Jan", u"Feb", u"Mar", u"Apr", u"May", u"Jun",
                                              u"Jul", u"Aug", u"Sep", u"Oct", u"Nov", u"Dec"};

constexpr DWORD LOCALE_NOUSEROVERRIDE = 0x80000000;
constexpr DWORD LOCALE_RETURN_NUMBER = 0x20000000;
constexpr DWORD LOCALE_USE_CP_ACP = 0x40000000;
constexpr DWORD LOCALE_ALLOW_NEUTRAL_NAMES = 0x08000000;

const char16_t* locale_string(DWORD type) {
  if (type >= 0x2A && type <= 0x30) return kDayNames[type - 0x2A];          // LOCALE_SDAYNAME1..7
  if (type >= 0x31 && type <= 0x37) return kDayAbbrev[type - 0x31];         // LOCALE_SABBREVDAYNAME1..7
  if (type >= 0x38 && type <= 0x43) return kMonthNames[type - 0x38];        // LOCALE_SMONTHNAME1..12
  if (type >= 0x44 && type <= 0x4F) return kMonthAbbrev[type - 0x44];       // LOCALE_SABBREVMONTHNAME1..12
  switch (type) {
    case 0x01: return u"0409";                 // LOCALE_ILANGUAGE
    case 0x02: return u"English (United States)";  // LOCALE_SLOCALIZEDDISPLAYNAME
    case 0x03: return u"ENU";                  // LOCALE_SABBREVLANGNAME
    case 0x04: return u"English";              // LOCALE_SNATIVELANGNAME
    case 0x05: return u"1";                    // LOCALE_ICOUNTRY
    case 0x06: return u"United States";        // LOCALE_SLOCALIZEDCOUNTRYNAME
    case 0x07: return u"USA";                  // LOCALE_SABBREVCTRYNAME
    case 0x08: return u"United States";        // LOCALE_SNATIVECOUNTRYNAME
    case 0x09: return u"0409";                 // LOCALE_IDEFAULTLANGUAGE
    case 0x0A: return u"1";                    // LOCALE_IDEFAULTCOUNTRY
    case 0x0B: return u"437";                  // LOCALE_IDEFAULTCODEPAGE
    case 0x0C: return u",";                    // LOCALE_SLIST
    case 0x0D: return u"1";                    // LOCALE_IMEASURE (US)
    case 0x0E: return u".";                    // LOCALE_SDECIMAL
    case 0x0F: return u",";                    // LOCALE_STHOUSAND
    case 0x10: return u"3;0";                  // LOCALE_SGROUPING
    case 0x11: return u"2";                    // LOCALE_IDIGITS
    case 0x12: return u"1";                    // LOCALE_ILZERO
    case 0x13: return u"0123456789";           // LOCALE_SNATIVEDIGITS
    case 0x14: return u"$";                    // LOCALE_SCURRENCY
    case 0x15: return u"USD";                  // LOCALE_SINTLSYMBOL
    case 0x16: return u".";                    // LOCALE_SMONDECIMALSEP
    case 0x17: return u",";                    // LOCALE_SMONTHOUSANDSEP
    case 0x18: return u"3;0";                  // LOCALE_SMONGROUPING
    case 0x19: return u"2";                    // LOCALE_ICURRDIGITS
    case 0x1A: return u"2";                    // LOCALE_IINTLCURRDIGITS
    case 0x1B: return u"0";                    // LOCALE_ICURRENCY
    case 0x1C: return u"0";                    // LOCALE_INEGCURR
    case 0x1D: return u"/";                    // LOCALE_SDATE
    case 0x1E: return u":";                    // LOCALE_STIME
    case 0x1F: return u"M/d/yyyy";             // LOCALE_SSHORTDATE
    case 0x20: return u"dddd, MMMM d, yyyy";   // LOCALE_SLONGDATE
    case 0x21: return u"0";                    // LOCALE_IDATE
    case 0x22: return u"0";                    // LOCALE_ILDATE
    case 0x23: return u"0";                    // LOCALE_ITIME (12h)
    case 0x24: return u"0";                    // LOCALE_ICENTURY
    case 0x25: return u"0";                    // LOCALE_ITLZERO
    case 0x26: return u"0";                    // LOCALE_IDAYLZERO
    case 0x27: return u"0";                    // LOCALE_IMONLZERO
    case 0x28: return u"AM";                   // LOCALE_S1159
    case 0x29: return u"PM";                   // LOCALE_S2359
    case 0x50: return u"";                     // LOCALE_SPOSITIVESIGN
    case 0x51: return u"-";                    // LOCALE_SNEGATIVESIGN
    case 0x52: return u"3";                    // LOCALE_IPOSSIGNPOSN
    case 0x53: return u"0";                    // LOCALE_INEGSIGNPOSN
    case 0x54: return u"1";                    // LOCALE_IPOSSYMPRECEDES
    case 0x55: return u"0";                    // LOCALE_IPOSSEPBYSPACE
    case 0x56: return u"1";                    // LOCALE_INEGSYMPRECEDES
    case 0x57: return u"0";                    // LOCALE_INEGSEPBYSPACE
    case 0x59: return u"en";                   // LOCALE_SISO639LANGNAME
    case 0x5A: return u"US";                   // LOCALE_SISO3166CTRYNAME
    case 0x5B: return u"";                     // LOCALE_SDEFAULTSCRIPT? (not used)
    case 0x5C: return u"en-US";                // LOCALE_SNAME
    case 0x5D: return u"0";                    // LOCALE_IDEFAULTMACCODEPAGE? (reported as 10000 on Windows)
    case 0x60: return u"0";                    // LOCALE_IDIGITSUBSTITUTION? keep plain digits
    case 0x67: return u"eng";                  // LOCALE_SISO639LANGNAME2
    case 0x68: return u"USA";                  // LOCALE_SISO3166CTRYNAME2
    case 0x69: return u"NaN";                  // LOCALE_SNAN
    case 0x6A: return u"Infinity";             // LOCALE_SPOSINFINITY
    case 0x6B: return u"-Infinity";            // LOCALE_SNEGINFINITY
    case 0x6C: return u"Latn;";                // LOCALE_SSCRIPTS
    case 0x6D: return u"en";                   // LOCALE_SPARENT
    case 0x6E: return u"";                     // LOCALE_SCONSOLEFALLBACKNAME
    case 0x70: return u"0";                    // LOCALE_IREADINGLAYOUT
    case 0x74: return u"1";                    // LOCALE_INEGATIVEPERCENT
    case 0x75: return u"0";                    // LOCALE_IPOSITIVEPERCENT
    case 0x76: return u"%";                    // LOCALE_SPERCENT
    case 0x77: return u"‰";               // LOCALE_SPERMILLE
    case 0x78: return u"MMMM d";               // LOCALE_SMONTHDAY
    case 0x79: return u"h:mm tt";              // LOCALE_SSHORTTIME
    case 0x7A: return u"en-US";                // LOCALE_SOPENTYPELANGUAGETAG
    case 0x7B: return u"en-US";                // LOCALE_SSORTLOCALE
    case 0x1001: return u"English";            // LOCALE_SENGLISHLANGUAGENAME
    case 0x1002: return u"United States";      // LOCALE_SENGLISHCOUNTRYNAME
    case 0x1003: return u"h:mm:ss tt";         // LOCALE_STIMEFORMAT
    case 0x1004: return u"1252";               // LOCALE_IDEFAULTANSICODEPAGE
    case 0x1005: return u"0";                  // LOCALE_ITIMEMARKPOSN
    case 0x1006: return u"M/d/yyyy";           // LOCALE_SYEARMONTH? (close enough)
    case 0x1007: return u"English";            // LOCALE_SENGCURRNAME? (US Dollar)
    case 0x1009: return u"1";                  // LOCALE_ICALENDARTYPE (Gregorian)
    case 0x100C: return u"6";                  // LOCALE_IFIRSTDAYOFWEEK (Sunday)
    case 0x100D: return u"0";                  // LOCALE_IFIRSTWEEKOFYEAR
    case 0x1010: return u"1";                  // LOCALE_INEGNUMBER
    case 0x1011: return u"en";                 // LOCALE_SPARENT alias
    case 0x1014: return u"0";                  // LOCALE_IDEFAULTEBCDICCODEPAGE
    case 0x1015: return u"3";                  // LOCALE_IPAPERSIZE (letter)
    case 0x1016: return u"0";                  // LOCALE_IOPTIONALCALENDAR
    case 0x1017: return u"0";                  // LOCALE_IPOSITIVEPERCENT alias
    case 0x1018: return u"English (United States)";  // LOCALE_SNATIVEDISPLAYNAME
    case 0x101A: return u"English (United States)";  // LOCALE_SENGLISHDISPLAYNAME
    case 0x1019: return u"0";                  // LOCALE_IGEOID? (244 = US)
    case 0x101B: return u"English (United States)";  // LOCALE_SLOCALIZEDLANGUAGENAME
    default: return nullptr;
  }
}

std::u16string current_locale_name() { return u"en-US"; }

int get_locale_info(std::u16string_view locale, DWORD type, LPWSTR data, int cch) {
  static std::atomic<bool> warned{false};
  if (!locale.empty() && locale != u"en-US" && locale != u"!x-sys-default-locale" && !warned.exchange(true))
    XBEM_INFO("kernelx", "locale '{}' requested - answering with en-US data", utf16_to_utf8(locale));
  const DWORD base = type & ~(LOCALE_NOUSEROVERRIDE | LOCALE_RETURN_NUMBER | LOCALE_USE_CP_ACP |
                              LOCALE_ALLOW_NEUTRAL_NAMES);
  const char16_t* value = locale_string(base);
  if (!value) {
    XBEM_WARN_ONCE("kernelx", "GetLocaleInfo: unsupported LCTYPE {:#x} (only the first is reported)", base);
    set_last_error(ERROR_INVALID_FLAGS);
    return 0;
  }
  if (type & LOCALE_RETURN_NUMBER) {
    // The value is returned as a DWORD in the buffer (cch counts WCHARs: 2).
    DWORD n = 0;
    for (const char16_t* c = value; *c >= u'0' && *c <= u'9'; ++c) n = n * 10 + (*c - u'0');
    if (base == 0x01 || base == 0x09) n = 0x0409;  // hexadecimal language ids
    if (cch == 0) return 2;
    if (!data || cch < 2) {
      set_last_error(ERROR_INSUFFICIENT_BUFFER);
      return 0;
    }
    std::memcpy(data, &n, 4);
    return 2;
  }
  return nls_copy_out(value, data, cch);
}

int XBEM_GUEST_ABI hle_GetLocaleInfoEx(LPCWSTR locale, DWORD type, LPWSTR data, int cch) {
  return get_locale_info(locale ? guest_wstr(locale) : std::u16string_view(), type, data, cch);
}

int XBEM_GUEST_ABI hle_GetLocaleInfoW(DWORD, DWORD type, LPWSTR data, int cch) {
  return get_locale_info({}, type, data, cch);
}

int XBEM_GUEST_ABI hle_GetLocaleInfoA(DWORD, DWORD type, LPSTR data, int cch) {
  char16_t wide[128];
  const int n = get_locale_info({}, type & ~LOCALE_RETURN_NUMBER, wide, 128);
  if (n <= 0) return 0;
  if (type & LOCALE_RETURN_NUMBER) return get_locale_info({}, type, reinterpret_cast<LPWSTR>(data), cch / 2) * 2;
  if (cch == 0) return n;
  if (!data || cch < n) {
    set_last_error(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  for (int i = 0; i < n; ++i) data[i] = static_cast<char>(wide[i] < 0x80 ? wide[i] : '?');
  return n;
}

struct LocaleId {
  const char16_t* name;
  DWORD lcid;
};
constexpr LocaleId kLocales[] = {
    {u"en-US", 0x0409}, {u"en-GB", 0x0809}, {u"en-CA", 0x1009}, {u"en-AU", 0x0C09}, {u"fr-FR", 0x040C},
    {u"fr-CA", 0x0C0C}, {u"de-DE", 0x0407}, {u"es-ES", 0x0C0A}, {u"es-MX", 0x080A}, {u"it-IT", 0x0410},
    {u"ja-JP", 0x0411}, {u"ko-KR", 0x0412}, {u"zh-CN", 0x0804}, {u"zh-TW", 0x0404}, {u"pt-BR", 0x0416},
    {u"pt-PT", 0x0816}, {u"ru-RU", 0x0419}, {u"pl-PL", 0x0415}, {u"cs-CZ", 0x0405}, {u"nl-NL", 0x0413},
    {u"sv-SE", 0x041D}, {u"da-DK", 0x0406}, {u"fi-FI", 0x040B}, {u"nb-NO", 0x0414}, {u"tr-TR", 0x041F},
    {u"ar-SA", 0x0401}, {u"he-IL", 0x040D}, {u"hu-HU", 0x040E}, {u"el-GR", 0x0408}, {u"en", 0x0009},
};

bool iequal16(std::u16string_view a, std::u16string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (fold_lower(a[i]) != fold_lower(b[i])) return false;
  return true;
}

DWORD XBEM_GUEST_ABI hle_LocaleNameToLCID(LPCWSTR name, DWORD) {
  const std::u16string_view n = name ? guest_wstr(name) : std::u16string_view();
  if (n.empty() || n == u"!x-sys-default-locale") return 0x0409;
  for (const LocaleId& l : kLocales)
    if (iequal16(n, l.name)) return l.lcid;
  return 0x1000;  // LOCALE_CUSTOM_UNSPECIFIED
}

int XBEM_GUEST_ABI hle_LCIDToLocaleName(DWORD lcid, LPWSTR name, int cch, DWORD) {
  if (lcid == 0x0400 || lcid == 0x0800 || lcid == 0) lcid = 0x0409;  // user/system default
  for (const LocaleId& l : kLocales)
    if (l.lcid == lcid) return nls_copy_out(l.name, name, cch);
  set_last_error(ERROR_INVALID_PARAMETER);
  return 0;
}

int XBEM_GUEST_ABI hle_GetUserDefaultGeoName(LPWSTR name, int cch) { return nls_copy_out(u"US", name, cch); }

int XBEM_GUEST_ABI hle_GetSystemDefaultLocaleName(LPWSTR name, int cch) {
  return nls_copy_out(current_locale_name(), name, cch);
}

BOOL XBEM_GUEST_ABI hle_IsValidLocaleName(LPCWSTR name) {
  return name && hle_LocaleNameToLCID(name, 0) != 0x1000 ? TRUE : FALSE;
}

// ------------------------------------------------- comparison and mapping

constexpr DWORD NORM_IGNORECASE = 0x1;
constexpr DWORD LINGUISTIC_IGNORECASE = 0x10;
constexpr DWORD NORM_LINGUISTIC_CASING = 0x08000000;
constexpr int CSTR_LESS_THAN = 1, CSTR_EQUAL = 2, CSTR_GREATER_THAN = 3;

int compare_strings(std::u16string_view a, std::u16string_view b, DWORD flags) {
  const bool ignore_case = flags & (NORM_IGNORECASE | LINGUISTIC_IGNORECASE);
  // Primary: case-insensitive; secondary (unless ignoring case): lower before upper.
  const size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    const char16_t x = fold_upper(a[i]), y = fold_upper(b[i]);
    if (x != y) return x < y ? CSTR_LESS_THAN : CSTR_GREATER_THAN;
  }
  if (a.size() != b.size()) return a.size() < b.size() ? CSTR_LESS_THAN : CSTR_GREATER_THAN;
  if (!ignore_case)
    for (size_t i = 0; i < n; ++i)
      if (a[i] != b[i]) return fold_lower(a[i]) == a[i] ? CSTR_LESS_THAN : CSTR_GREATER_THAN;
  return CSTR_EQUAL;
}

int XBEM_GUEST_ABI hle_CompareStringEx(LPCWSTR, DWORD flags, LPCWSTR s1, int n1, LPCWSTR s2, int n2, void*, void*,
                                       uint64_t) {
  if (!s1 || !s2) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return 0;
  }
  return compare_strings(wview(s1, n1), wview(s2, n2), flags);
}

int XBEM_GUEST_ABI hle_CompareStringW(DWORD, DWORD flags, LPCWSTR s1, int n1, LPCWSTR s2, int n2) {
  return hle_CompareStringEx(nullptr, flags, s1, n1, s2, n2, nullptr, nullptr, 0);
}

int XBEM_GUEST_ABI hle_lstrcmpW(LPCWSTR a, LPCWSTR b) {
  return compare_strings(a ? guest_wstr(a) : u"", b ? guest_wstr(b) : u"", 0) - 2;
}
int XBEM_GUEST_ABI hle_lstrcmpiW(LPCWSTR a, LPCWSTR b) {
  return compare_strings(a ? guest_wstr(a) : u"", b ? guest_wstr(b) : u"", NORM_IGNORECASE) - 2;
}
int XBEM_GUEST_ABI hle_lstrcmpA(LPCSTR a, LPCSTR b) {
  return std::clamp(std::string_view(a ? a : "").compare(b ? b : ""), -1, 1);
}
int XBEM_GUEST_ABI hle_lstrcmpiA(LPCSTR a, LPCSTR b) {
  const std::u16string x = utf8_to_utf16(a ? a : ""), y = utf8_to_utf16(b ? b : "");
  return compare_strings(x, y, NORM_IGNORECASE) - 2;
}

constexpr DWORD LCMAP_LOWERCASE = 0x100;
constexpr DWORD LCMAP_UPPERCASE = 0x200;
constexpr DWORD LCMAP_SORTKEY = 0x400;

int XBEM_GUEST_ABI hle_LCMapStringEx(LPCWSTR, DWORD flags, LPCWSTR src, int src_len, LPWSTR dst, int dst_len, void*,
                                     void*, uint64_t) {
  if (!src) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return 0;
  }
  const std::u16string_view in = wview(src, src_len);
  const bool with_nul = src_len < 0;
  if (flags & LCMAP_SORTKEY) {
    // Byte sort key that orders like compare_strings with NORM_IGNORECASE
    // semantics as primary weight: upper-cased UTF-16BE, then a terminator.
    std::string key;
    for (char16_t c : in) {
      const char16_t u = fold_upper(c);
      key.push_back(static_cast<char>((u >> 8) + 1));  // never 0 inside the key
      key.push_back(static_cast<char>(u & 0xFF));
    }
    key.push_back('\1');
    key.push_back('\0');
    const int need = static_cast<int>(key.size());
    if (dst_len == 0) return need;
    if (!dst || dst_len < need) {
      set_last_error(ERROR_INSUFFICIENT_BUFFER);
      return 0;
    }
    std::memcpy(dst, key.data(), key.size());  // dst is a byte buffer for sort keys
    return need;
  }
  std::u16string out(in);
  if (flags & LCMAP_LOWERCASE)
    for (auto& c : out) c = fold_lower(c);
  if (flags & LCMAP_UPPERCASE)
    for (auto& c : out) c = fold_upper(c);
  const int need = static_cast<int>(out.size()) + (with_nul ? 1 : 0);
  if (dst_len == 0) return need;
  if (!dst || dst_len < need) {
    set_last_error(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  std::copy(out.begin(), out.end(), dst);
  if (with_nul) dst[out.size()] = 0;
  return need;
}

int XBEM_GUEST_ABI hle_LCMapStringW(DWORD, DWORD flags, LPCWSTR src, int src_len, LPWSTR dst, int dst_len) {
  return hle_LCMapStringEx(nullptr, flags, src, src_len, dst, dst_len, nullptr, nullptr, 0);
}

// CT_CTYPE1 classification.
WORD ctype1(char16_t c) {
  constexpr WORD C1_UPPER = 0x1, C1_LOWER = 0x2, C1_DIGIT = 0x4, C1_SPACE = 0x8, C1_PUNCT = 0x10, C1_CNTRL = 0x20,
                 C1_BLANK = 0x40, C1_XDIGIT = 0x80, C1_ALPHA = 0x100, C1_DEFINED = 0x200;
  WORD t = C1_DEFINED;
  if (c < 0x20 || c == 0x7F) t |= C1_CNTRL;
  if (c == u' ' || c == u'\t') t |= C1_BLANK | C1_SPACE;
  if (c == u'\n' || c == u'\r' || c == u'\v' || c == u'\f') t |= C1_SPACE;
  if (c >= u'0' && c <= u'9') t |= C1_DIGIT | C1_XDIGIT;
  if ((c >= u'a' && c <= u'f') || (c >= u'A' && c <= u'F')) t |= C1_XDIGIT;
  const bool upper = (c >= u'A' && c <= u'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7);
  const bool lower = (c >= u'a' && c <= u'z') || (c >= 0xDF && c <= 0xFF && c != 0xF7);
  if (upper) t |= C1_UPPER | C1_ALPHA;
  if (lower) t |= C1_LOWER | C1_ALPHA;
  if (c >= 0x100 && !(c >= 0x2000 && c <= 0x206F) && !(c >= 0x3000 && c <= 0x303F)) t |= C1_ALPHA;
  if ((c > 0x20 && c < 0x7F && !(t & (C1_ALPHA | C1_DIGIT))) || (c >= 0xA1 && c <= 0xBF) || c == 0xD7 || c == 0xF7 ||
      (c >= 0x2000 && c <= 0x206F) || (c >= 0x3000 && c <= 0x303F))
    t |= C1_PUNCT;
  if (c == 0xA0) t |= C1_BLANK | C1_SPACE;
  return t;
}

BOOL get_string_type(DWORD info_type, LPCWSTR src, int count, WORD* out) {
  if (!src || !out) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  const std::u16string_view s = count < 0 ? std::u16string_view(src, guest_wstr(src).size() + 1)
                                          : std::u16string_view(src, static_cast<size_t>(count));
  for (size_t i = 0; i < s.size(); ++i) {
    const WORD c1 = ctype1(s[i]);
    switch (info_type) {
      case 1: out[i] = c1; break;                                                  // CT_CTYPE1
      case 2: out[i] = (c1 & 0x100) ? 1 /*C2_LEFTTORIGHT*/ : (c1 & 0x4) ? 3 /*C2_EUROPENUMBER*/
                                                                     : (c1 & 0x8) ? 10 /*C2_WHITESPACE*/ : 11;
        break;                                                                      // CT_CTYPE2
      case 4: out[i] = (c1 & 0x100) ? 0x8000 /*C3_ALPHA*/ : 0; break;               // CT_CTYPE3
      default: set_last_error(ERROR_INVALID_FLAGS); return FALSE;
    }
  }
  return TRUE;
}

BOOL XBEM_GUEST_ABI hle_GetStringTypeW(DWORD info_type, LPCWSTR src, int count, WORD* out) {
  return get_string_type(info_type, src, count, out);
}
BOOL XBEM_GUEST_ABI hle_GetStringTypeExW(DWORD, DWORD info_type, LPCWSTR src, int count, WORD* out) {
  return get_string_type(info_type, src, count, out);
}

// ------------------------------------------------- date/time formatting

struct SystemTime {
  WORD year, month, day_of_week, day, hour, minute, second, ms;
};

std::u16string format_picture(std::u16string_view pic, const SystemTime& t, bool date, DWORD time_flags) {
  std::u16string out;
  auto num = [&](int v, int width) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%0*d", width, v);
    for (const char* c = buf; *c; ++c) out.push_back(static_cast<char16_t>(*c));
  };
  for (size_t i = 0; i < pic.size();) {
    const char16_t c = pic[i];
    if (c == u'\'') {  // quoted literal; '' is a single quote
      size_t j = i + 1;
      if (j < pic.size() && pic[j] == u'\'') {
        out.push_back(u'\'');
        i += 2;
        continue;
      }
      while (j < pic.size() && pic[j] != u'\'') out.push_back(pic[j++]);
      i = j + 1;
      continue;
    }
    size_t run = 1;
    while (i + run < pic.size() && pic[i + run] == c) ++run;
    if (date && (c == u'd' || c == u'M' || c == u'y' || c == u'g')) {
      if (c == u'd') {
        if (run == 1) num(t.day, 1);
        else if (run == 2) num(t.day, 2);
        else if (run == 3) out += kDayAbbrev[(t.day_of_week + 6) % 7];
        else out += kDayNames[(t.day_of_week + 6) % 7];
      } else if (c == u'M') {
        const int m = std::clamp<int>(t.month, 1, 12) - 1;
        if (run == 1) num(t.month, 1);
        else if (run == 2) num(t.month, 2);
        else if (run == 3) out += kMonthAbbrev[m];
        else out += kMonthNames[m];
      } else if (c == u'y') {
        if (run <= 2) num(t.year % 100, run);
        else num(t.year, 4);
      } else {
        out += u"A.D.";
      }
    } else if (!date && (c == u'h' || c == u'H' || c == u'm' || c == u's' || c == u't')) {
      const bool force24 = time_flags & 0x8;
      if (c == u'h' && !force24) num(t.hour % 12 == 0 ? 12 : t.hour % 12, std::min<int>(static_cast<int>(run), 2));
      else if (c == u'h' || c == u'H') num(t.hour, std::min<int>(static_cast<int>(run), 2));
      else if (c == u'm') {
        if (time_flags & 0x1) {  // TIME_NOMINUTESORSECONDS: drop with the preceding separator
          while (!out.empty() && (out.back() == u':' || out.back() == u'.')) out.pop_back();
        } else {
          num(t.minute, std::min<int>(static_cast<int>(run), 2));
        }
      } else if (c == u's') {
        if (time_flags & 0x3) {
          while (!out.empty() && (out.back() == u':' || out.back() == u'.')) out.pop_back();
        } else {
          num(t.second, std::min<int>(static_cast<int>(run), 2));
        }
      } else if (c == u't') {
        if (!(time_flags & 0x4) && !force24) {
          const char16_t* marker = t.hour < 12 ? u"AM" : u"PM";
          out += run == 1 ? std::u16string(1, marker[0]) : std::u16string(marker);
        } else {
          while (!out.empty() && out.back() == u' ') out.pop_back();
        }
      }
    } else {
      out.append(run, c);
    }
    i += run;
  }
  return out;
}

int format_date_time(const void* st, LPCWSTR format, LPWSTR out, int cch, bool date, DWORD flags) {
  SystemTime t;
  if (st) std::memcpy(&t, st, sizeof t);
  else fill_local_systemtime(&t);
  std::u16string_view pic;
  if (format) pic = guest_wstr(format);
  else if (date) pic = (flags & 0x2 /*DATE_LONGDATE*/) ? u"dddd, MMMM d, yyyy" : u"M/d/yyyy";
  else pic = u"h:mm:ss tt";
  return nls_copy_out(format_picture(pic, t, date, date ? 0 : flags), out, cch);
}

int XBEM_GUEST_ABI hle_GetDateFormatEx(LPCWSTR, DWORD flags, const void* st, LPCWSTR format, LPWSTR out, int cch,
                                       LPCWSTR) {
  return format_date_time(st, format, out, cch, true, flags);
}
int XBEM_GUEST_ABI hle_GetTimeFormatEx(LPCWSTR, DWORD flags, const void* st, LPCWSTR format, LPWSTR out, int cch) {
  return format_date_time(st, format, out, cch, false, flags);
}
int XBEM_GUEST_ABI hle_GetDateFormatW(DWORD, DWORD flags, const void* st, LPCWSTR format, LPWSTR out, int cch) {
  return format_date_time(st, format, out, cch, true, flags);
}
int XBEM_GUEST_ABI hle_GetTimeFormatW(DWORD, DWORD flags, const void* st, LPCWSTR format, LPWSTR out, int cch) {
  return format_date_time(st, format, out, cch, false, flags);
}

// ------------------------------------------------------- FormatMessage

constexpr DWORD FORMAT_MESSAGE_ALLOCATE_BUFFER = 0x100;
constexpr DWORD FORMAT_MESSAGE_IGNORE_INSERTS = 0x200;
constexpr DWORD FORMAT_MESSAGE_FROM_STRING = 0x400;
constexpr DWORD FORMAT_MESSAGE_FROM_SYSTEM = 0x1000;

const char* system_message(DWORD id) {
  switch (id) {
    case 0: return "The operation completed successfully.";
    case 1: return "Incorrect function.";
    case 2: return "The system cannot find the file specified.";
    case 3: return "The system cannot find the path specified.";
    case 4: return "The system cannot open the file.";
    case 5: return "Access is denied.";
    case 6: return "The handle is invalid.";
    case 8: return "Not enough memory resources are available to process this command.";
    case 13: return "The data is invalid.";
    case 14: return "Not enough memory resources are available to complete this operation.";
    case 18: return "There are no more files.";
    case 32: return "The process cannot access the file because it is being used by another process.";
    case 38: return "Reached the end of the file.";
    case 50: return "The request is not supported.";
    case 80: return "The file exists.";
    case 87: return "The parameter is incorrect.";
    case 109: return "The pipe has been ended.";
    case 112: return "There is not enough space on the disk.";
    case 120: return "This function is not supported on this system.";
    case 122: return "The data area passed to a system call is too small.";
    case 126: return "The specified module could not be found.";
    case 127: return "The specified procedure could not be found.";
    case 183: return "Cannot create a file when that file already exists.";
    case 258: return "The wait operation timed out.";
    case 259: return "No more data is available.";
    case 487: return "Attempt to access invalid address.";
    case 995: return "The I/O operation has been aborted because of either a thread exit or an application request.";
    case 997: return "Overlapped I/O operation is in progress.";
    case 1168: return "Element not found.";
    case 1460: return "This operation returned because the timeout period expired.";
    case 10050: return "A socket operation encountered a dead network.";
    case 11001: return "No such host is known.";
    case 12029: return "A connection with the server could not be established";
    case 0x80004001: return "Not implemented";
    case 0x80004002: return "No such interface supported";
    case 0x80004005: return "Unspecified error";
    case 0x8007000E: return "Not enough memory resources are available to complete this operation.";
    case 0x80070057: return "The parameter is incorrect.";
    default: return nullptr;
  }
}

template <class Char>
std::u16string arg_string(uint64_t v) {
  if (!v) return u"(null)";
  if constexpr (sizeof(Char) == 2) return std::u16string(guest_wstr(reinterpret_cast<const char16_t*>(v)));
  else return utf8_to_utf16(guest_cstr(reinterpret_cast<const char*>(v)));
}

// Expands %1..%99 (with an optional !printf-format!), %n, %%, %0 and friends.
template <class Char>
std::u16string expand_message(std::u16string_view text, bool ignore_inserts, const uint64_t* args) {
  std::u16string out;
  for (size_t i = 0; i < text.size(); ++i) {
    const char16_t c = text[i];
    if (c != u'%' || i + 1 >= text.size()) {
      out.push_back(c);
      continue;
    }
    const char16_t n = text[i + 1];
    if (n >= u'1' && n <= u'9') {
      size_t j = i + 1;
      int index = 0;
      while (j < text.size() && text[j] >= u'0' && text[j] <= u'9' && index < 10) index = index * 10 + (text[j++] - u'0');
      std::u16string spec = u"s";
      if (j < text.size() && text[j] == u'!') {
        const size_t end = text.find(u'!', j + 1);
        if (end != std::u16string_view::npos) {
          spec = std::u16string(text.substr(j + 1, end - j - 1));
          j = end + 1;
        }
      }
      if (ignore_inserts || !args) {
        out.append(text.substr(i, j - i));
      } else {
        const uint64_t v = args[index - 1];
        const char16_t conv = spec.empty() ? u's' : spec.back();
        char buf[64];
        switch (conv) {
          case u's': out += (spec.find(u'S') != std::u16string::npos) ? arg_string<char>(v) : arg_string<Char>(v); break;
          case u'S': out += sizeof(Char) == 2 ? arg_string<char>(v) : arg_string<char16_t>(v); break;
          case u'd':
          case u'i':
            std::snprintf(buf, sizeof buf, "%lld",
                          spec.find(u"64") != std::u16string::npos ? static_cast<long long>(v)
                                                                   : static_cast<long long>(static_cast<int32_t>(v)));
            out += utf8_to_utf16(buf);
            break;
          case u'u':
            std::snprintf(buf, sizeof buf, "%llu",
                          spec.find(u"64") != std::u16string::npos ? static_cast<unsigned long long>(v)
                                                                   : static_cast<unsigned long long>(static_cast<uint32_t>(v)));
            out += utf8_to_utf16(buf);
            break;
          case u'x':
          case u'X':
          case u'p':
            std::snprintf(buf, sizeof buf, conv == u'X' ? "%llX" : "%llx",
                          static_cast<unsigned long long>(spec.find(u"64") != std::u16string::npos || conv == u'p'
                                                              ? v
                                                              : static_cast<uint32_t>(v)));
            out += utf8_to_utf16(buf);
            break;
          case u'c': out.push_back(static_cast<char16_t>(v)); break;
          default: out += arg_string<Char>(v); break;
        }
      }
      i = j - 1;
      continue;
    }
    ++i;
    switch (n) {
      case u'0': return out;  // end of message, no trailing newline
      case u'n': out += u"\r\n"; break;
      case u'r': out.push_back(u'\r'); break;
      case u't': out.push_back(u'\t'); break;
      case u'b': out.push_back(u' '); break;
      default: out.push_back(n); break;  // %% %. %! %space
    }
  }
  return out;
}

template <class Char>
DWORD format_message(DWORD flags, const void* source, DWORD id, Char* buffer, DWORD size, void* arguments) {
  std::u16string text;
  if (flags & FORMAT_MESSAGE_FROM_STRING) {
    if (!source) {
      set_last_error(ERROR_INVALID_PARAMETER);
      return 0;
    }
    if constexpr (sizeof(Char) == 2) text = std::u16string(guest_wstr(static_cast<const char16_t*>(source)));
    else text = utf8_to_utf16(guest_cstr(static_cast<const char*>(source)));
  } else if (flags & FORMAT_MESSAGE_FROM_SYSTEM) {
    const char* m = system_message(id);
    if (!m) {
      set_last_error(ERROR_MR_MID_NOT_FOUND);
      return 0;
    }
    text = utf8_to_utf16(m) + u"%n";
  } else {
    // FORMAT_MESSAGE_FROM_HMODULE: message tables are not supported.
    set_last_error(ERROR_MR_MID_NOT_FOUND);
    return 0;
  }
  const uint64_t* args = nullptr;
  if (arguments) {
    args = (flags & 0x2000 /*FORMAT_MESSAGE_ARGUMENT_ARRAY*/)
               ? static_cast<const uint64_t*>(arguments)
               : *static_cast<const uint64_t* const*>(arguments);  // MS va_list: pointer to 8-byte slots
  }
  const std::u16string msg = expand_message<Char>(text, flags & FORMAT_MESSAGE_IGNORE_INSERTS, args);
  std::basic_string<Char> result;
  if constexpr (sizeof(Char) == 2) result = msg;
  else result = utf16_to_utf8(msg);

  if (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER) {
    const size_t n = std::max<size_t>(result.size() + 1, size);
    auto* mem = static_cast<Char*>(heap_alloc(n * sizeof(Char), true));  // released with LocalFree
    if (!mem) {
      set_last_error(ERROR_NOT_ENOUGH_MEMORY);
      return 0;
    }
    std::memcpy(mem, result.data(), result.size() * sizeof(Char));
    *reinterpret_cast<Char**>(buffer) = mem;
    return static_cast<DWORD>(result.size());
  }
  if (!buffer || size < result.size() + 1) {
    set_last_error(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  std::memcpy(buffer, result.data(), result.size() * sizeof(Char));
  buffer[result.size()] = 0;
  return static_cast<DWORD>(result.size());
}

DWORD XBEM_GUEST_ABI hle_FormatMessageW(DWORD flags, const void* source, DWORD id, DWORD, LPWSTR buffer, DWORD size,
                                        void* args) {
  return format_message<char16_t>(flags, source, id, buffer, size, args);
}
DWORD XBEM_GUEST_ABI hle_FormatMessageA(DWORD flags, const void* source, DWORD id, DWORD, LPSTR buffer, DWORD size,
                                        void* args) {
  return format_message<char>(flags, source, id, buffer, size, args);
}

}  // namespace

void add_kernelx_locale_exports(std::vector<ExportDef>& out) {
  out.insert(out.end(), {
      XBEM_HLE(GetLocaleInfoEx),      XBEM_HLE(GetLocaleInfoW),       XBEM_HLE(GetLocaleInfoA),
      XBEM_HLE(LocaleNameToLCID),     XBEM_HLE(LCIDToLocaleName),     XBEM_HLE(GetUserDefaultGeoName),
      XBEM_HLE(GetSystemDefaultLocaleName), XBEM_HLE(IsValidLocaleName), XBEM_HLE(CompareStringEx),
      XBEM_HLE(CompareStringW),       XBEM_HLE(lstrcmpW),             XBEM_HLE(lstrcmpiW),
      XBEM_HLE(lstrcmpA),             XBEM_HLE(lstrcmpiA),            XBEM_HLE(LCMapStringEx),
      XBEM_HLE(LCMapStringW),         XBEM_HLE(GetStringTypeW),       XBEM_HLE(GetStringTypeExW),
      XBEM_HLE(GetDateFormatEx),      XBEM_HLE(GetTimeFormatEx),      XBEM_HLE(GetDateFormatW),
      XBEM_HLE(GetTimeFormatW),       XBEM_HLE(FormatMessageW),       XBEM_HLE(FormatMessageA),
  });
}

}  // namespace xbem::hle
