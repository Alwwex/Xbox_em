// UCRT: narrow and wide string functions, character classification,
// number conversion and the "C" locale.
//
// Guest wchar_t is 16 bits (char16_t) and guest `long` is 32 bits, so wide
// functions and anything returning `long` are implemented here instead of
// forwarding to the host C library.

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "hle/heap.h"
#include "hle/ucrt.h"

namespace xbem::hle {
namespace {

using namespace win;
using crt::set_errno;
using wchar = char16_t;

constexpr SIZE_T kTruncate = ~SIZE_T(0);  // _TRUNCATE

// ------------------------------------------------------------ ctype table
// MSVC classification bits.
constexpr uint16_t kUpper = 0x1, kLower = 0x2, kDigit = 0x4, kSpace = 0x8, kPunct = 0x10, kControl = 0x20,
                   kBlank = 0x40, kHex = 0x80, kAlpha = 0x100 | kUpper | kLower;

struct CtypeTables {
  uint16_t narrow[257];  // index 0 is EOF (-1)
  uint16_t wide[256];
  CtypeTables() {
    for (int c = 0; c < 256; ++c) {
      uint16_t v = 0;
      if (c < 0x20 || c == 0x7F) v |= kControl;
      if ((c >= 9 && c <= 13) || c == ' ') v |= kSpace;
      if (c == ' ' || c == '\t') v |= kBlank;
      if (c >= '0' && c <= '9') v |= kDigit | kHex;
      if (c >= 'A' && c <= 'Z') v |= 0x100 | kUpper;
      if (c >= 'a' && c <= 'z') v |= 0x100 | kLower;
      if ((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) v |= kHex;
      if ((c >= 33 && c <= 47) || (c >= 58 && c <= 64) || (c >= 91 && c <= 96) || (c >= 123 && c <= 126)) v |= kPunct;
      narrow[c + 1] = c < 0x80 ? v : 0;  // "C" locale: only ASCII is classified
      // Wide classification also knows Latin-1.
      uint16_t w = v;
      if (c >= 0x80 && c < 0xA0) w = kControl;
      if (c == 0xA0) w = kSpace | kBlank;
      if (c >= 0xC0 && c <= 0xDE && c != 0xD7) w = 0x100 | kUpper;
      if (c >= 0xDF && c != 0xF7) w = 0x100 | kLower;
      if ((c >= 0xA1 && c <= 0xBF) || c == 0xD7 || c == 0xF7) w = kPunct;
      wide[c] = w;
    }
    narrow[0] = 0;
  }
};

const CtypeTables& ctype() {
  static const CtypeTables t;
  return t;
}

uint16_t class_of(int32_t c) { return (c >= -1 && c <= 255) ? ctype().narrow[c + 1] : 0; }

uint16_t wclass_of(uint32_t c) {
  if (c < 256) return ctype().wide[c];
  if (c == 0x2028 || c == 0x2029 || c == 0x3000 || (c >= 0x2000 && c <= 0x200A)) return kSpace;
  if (c >= 0xD800 && c <= 0xDFFF) return 0;
  return 0x100;  // treat everything else as a letter (good enough for identifiers/text)
}

int32_t to_upper_ascii(int32_t c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
int32_t to_lower_ascii(int32_t c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

uint32_t wto_upper(uint32_t c) {
  if (c >= 'a' && c <= 'z') return c - 32;
  if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
  if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 32;  // Greek
  if (c >= 0x430 && c <= 0x44F) return c - 32;                // Cyrillic
  if (c >= 0x450 && c <= 0x45F) return c - 80;
  return c;
}

uint32_t wto_lower(uint32_t c) {
  if (c >= 'A' && c <= 'Z') return c + 32;
  if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
  if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;
  if (c >= 0x410 && c <= 0x42F) return c + 32;
  if (c >= 0x400 && c <= 0x40F) return c + 80;
  return c;
}

#define XBEM_CTYPE(name, mask)                                                        \
  int32_t XBEM_GUEST_ABI hle_##name(int32_t c) { return class_of(c) & (mask); }       \
  int32_t XBEM_GUEST_ABI hle__##name##_l(int32_t c, void*) { return class_of(c) & (mask); } \
  int32_t XBEM_GUEST_ABI hle_isw##name##_impl(uint32_t c) { return wclass_of(c & 0xFFFF) & (mask); }

XBEM_CTYPE(isalpha, kAlpha)
XBEM_CTYPE(isupper, kUpper)
XBEM_CTYPE(islower, kLower)
XBEM_CTYPE(isdigit, kDigit)
XBEM_CTYPE(isxdigit, kHex)
XBEM_CTYPE(isspace, kSpace)
XBEM_CTYPE(ispunct, kPunct)
XBEM_CTYPE(isalnum, kAlpha | kDigit)
XBEM_CTYPE(isgraph, kAlpha | kDigit | kPunct)
XBEM_CTYPE(iscntrl, kControl)
XBEM_CTYPE(isblank, kBlank)

int32_t XBEM_GUEST_ABI hle_isprint_fix(int32_t c) { return c == ' ' || (class_of(c) & (kAlpha | kDigit | kPunct)); }
int32_t XBEM_GUEST_ABI hle__isprint_l(int32_t c, void*) { return hle_isprint_fix(c); }

int32_t XBEM_GUEST_ABI hle_toupper(int32_t c) { return to_upper_ascii(c); }
int32_t XBEM_GUEST_ABI hle_tolower(int32_t c) { return to_lower_ascii(c); }
int32_t XBEM_GUEST_ABI hle__toupper_l(int32_t c, void*) { return to_upper_ascii(c); }
int32_t XBEM_GUEST_ABI hle__tolower_l(int32_t c, void*) { return to_lower_ascii(c); }
int32_t XBEM_GUEST_ABI hle__toupper(int32_t c) { return c - 'a' + 'A'; }
int32_t XBEM_GUEST_ABI hle__tolower(int32_t c) { return c - 'A' + 'a'; }
int32_t XBEM_GUEST_ABI hle___isascii(int32_t c) { return static_cast<uint32_t>(c) < 0x80; }
int32_t XBEM_GUEST_ABI hle___toascii(int32_t c) { return c & 0x7F; }
int32_t XBEM_GUEST_ABI hle___iscsym(int32_t c) { return (class_of(c) & (kAlpha | kDigit)) || c == '_'; }
int32_t XBEM_GUEST_ABI hle___iscsymf(int32_t c) { return (class_of(c) & kAlpha) || c == '_'; }
int32_t XBEM_GUEST_ABI hle__isctype(int32_t c, int32_t mask) { return class_of(c) & mask; }
int32_t XBEM_GUEST_ABI hle__isctype_l(int32_t c, int32_t mask, void*) { return class_of(c) & mask; }
const uint16_t* XBEM_GUEST_ABI hle___pctype_func() { return ctype().narrow + 1; }
const uint16_t* XBEM_GUEST_ABI hle___pwctype_func() { return ctype().wide; }

// Wide classification (wint_t arguments are 16-bit values in a 32-bit slot).
int32_t XBEM_GUEST_ABI hle_iswalpha(uint32_t c) { return hle_iswisalpha_impl(c); }
int32_t XBEM_GUEST_ABI hle_iswupper(uint32_t c) { return hle_iswisupper_impl(c); }
int32_t XBEM_GUEST_ABI hle_iswlower(uint32_t c) { return hle_iswislower_impl(c); }
int32_t XBEM_GUEST_ABI hle_iswdigit(uint32_t c) { return hle_iswisdigit_impl(c); }
int32_t XBEM_GUEST_ABI hle_iswxdigit(uint32_t c) { return hle_iswisxdigit_impl(c); }
int32_t XBEM_GUEST_ABI hle_iswspace(uint32_t c) { return hle_iswisspace_impl(c); }
int32_t XBEM_GUEST_ABI hle_iswpunct(uint32_t c) { return hle_iswispunct_impl(c); }
int32_t XBEM_GUEST_ABI hle_iswalnum(uint32_t c) { return hle_iswisalnum_impl(c); }
int32_t XBEM_GUEST_ABI hle_iswprint(uint32_t c) { return (c & 0xFFFF) == ' ' || hle_iswisgraph_impl(c); }
int32_t XBEM_GUEST_ABI hle_iswgraph(uint32_t c) { return hle_iswisgraph_impl(c); }
int32_t XBEM_GUEST_ABI hle_iswcntrl(uint32_t c) { return hle_iswiscntrl_impl(c); }
int32_t XBEM_GUEST_ABI hle_iswblank(uint32_t c) { return hle_iswisblank_impl(c); }
int32_t XBEM_GUEST_ABI hle_iswascii(uint32_t c) { return (c & 0xFFFF) < 0x80; }
int32_t XBEM_GUEST_ABI hle_iswctype(uint32_t c, uint16_t mask) { return wclass_of(c & 0xFFFF) & mask; }
int32_t XBEM_GUEST_ABI hle__iswctype_l(uint32_t c, uint16_t mask, void*) { return wclass_of(c & 0xFFFF) & mask; }
uint32_t XBEM_GUEST_ABI hle_towupper(uint32_t c) { return wto_upper(c & 0xFFFF); }
uint32_t XBEM_GUEST_ABI hle_towlower(uint32_t c) { return wto_lower(c & 0xFFFF); }
uint32_t XBEM_GUEST_ABI hle__towupper_l(uint32_t c, void*) { return wto_upper(c & 0xFFFF); }
uint32_t XBEM_GUEST_ABI hle__towlower_l(uint32_t c, void*) { return wto_lower(c & 0xFFFF); }
int32_t XBEM_GUEST_ABI hle_isleadbyte(int32_t) { return 0; }
int32_t XBEM_GUEST_ABI hle__isleadbyte_l(int32_t, void*) { return 0; }

// ------------------------------------------------------- generic helpers

template <class C>
size_t slen(const C* s) {
  size_t n = 0;
  while (s[n]) ++n;
  return n;
}

template <class C>
int scmp(const C* a, const C* b) {
  using U = std::make_unsigned_t<C>;
  while (*a && *a == *b) ++a, ++b;
  return static_cast<int>(static_cast<U>(*a)) - static_cast<int>(static_cast<U>(*b));
}

template <class C>
int sncmp(const C* a, const C* b, size_t n) {
  using U = std::make_unsigned_t<C>;
  for (; n; --n, ++a, ++b) {
    if (*a != *b) return static_cast<int>(static_cast<U>(*a)) - static_cast<int>(static_cast<U>(*b));
    if (!*a) return 0;
  }
  return 0;
}

template <class C>
int sicmp(const C* a, const C* b, size_t n = ~size_t(0)) {
  for (; n; --n, ++a, ++b) {
    const int x = to_lower_ascii(static_cast<std::make_unsigned_t<C>>(*a));
    const int y = to_lower_ascii(static_cast<std::make_unsigned_t<C>>(*b));
    if (x != y) return x - y;
    if (!x) return 0;
  }
  return 0;
}

template <class C>
C* schr(const C* s, uint32_t c) {
  for (;; ++s) {
    if (static_cast<std::make_unsigned_t<C>>(*s) == static_cast<std::make_unsigned_t<C>>(c)) return const_cast<C*>(s);
    if (!*s) return nullptr;
  }
}

template <class C>
C* srchr(const C* s, uint32_t c) {
  const C* last = nullptr;
  for (;; ++s) {
    if (static_cast<std::make_unsigned_t<C>>(*s) == static_cast<std::make_unsigned_t<C>>(c)) last = s;
    if (!*s) return const_cast<C*>(last);
  }
}

template <class C>
C* sstr(const C* hay, const C* needle) {
  const size_t n = slen(needle);
  if (!n) return const_cast<C*>(hay);
  for (; *hay; ++hay)
    if (sncmp(hay, needle, n) == 0) return const_cast<C*>(hay);
  return nullptr;
}

template <class C>
size_t sspn(const C* s, const C* accept, bool reject) {
  size_t n = 0;
  for (; s[n]; ++n) {
    const bool in = schr(accept, static_cast<std::make_unsigned_t<C>>(s[n])) != nullptr;
    if (in == reject) break;
  }
  return n;
}

template <class C>
C* spbrk(const C* s, const C* accept) {
  for (; *s; ++s)
    if (schr(accept, static_cast<std::make_unsigned_t<C>>(*s))) return const_cast<C*>(s);
  return nullptr;
}

template <class C>
C* stok(C* str, const C* delim, C** context) {
  C* s = str ? str : *context;
  if (!s) return nullptr;
  s += sspn(s, delim, false);
  if (!*s) {
    *context = s;
    return nullptr;
  }
  C* end = s + sspn(s, delim, true);
  if (*end) *end++ = 0;
  *context = end;
  return s;
}

// --------------------------------------------------- secure (_s) copies

template <class C>
int32_t copy_s(C* dst, SIZE_T size, const C* src, SIZE_T count, const char* fn) {
  if (!dst || size == 0) {
    crt::invalid_parameter(fn);
    return crt::kEINVAL;
  }
  if (!src) {
    dst[0] = 0;
    crt::invalid_parameter(fn);
    return crt::kEINVAL;
  }
  size_t n = 0;
  while (n < count && src[n]) ++n;
  if (n < size) {
    std::memmove(dst, src, n * sizeof(C));
    dst[n] = 0;
    return 0;
  }
  if (count == kTruncate) {
    std::memmove(dst, src, (size - 1) * sizeof(C));
    dst[size - 1] = 0;
    return crt::kSTRUNCATE;
  }
  dst[0] = 0;
  crt::invalid_parameter(fn);
  return crt::kERANGE;
}

template <class C>
int32_t cat_s(C* dst, SIZE_T size, const C* src, SIZE_T count, const char* fn) {
  if (!dst || size == 0) {
    crt::invalid_parameter(fn);
    return crt::kEINVAL;
  }
  size_t len = 0;
  while (len < size && dst[len]) ++len;
  if (len == size) {
    dst[0] = 0;
    crt::invalid_parameter(fn);
    return crt::kEINVAL;
  }
  const int32_t r = copy_s(dst + len, size - len, src, count, fn);
  if (r == crt::kERANGE || r == crt::kEINVAL) dst[0] = 0;
  return r;
}

template <class C>
int32_t case_s(C* s, SIZE_T size, bool upper) {
  if (!s) return crt::kEINVAL;
  size_t i = 0;
  for (; i < size && s[i]; ++i) s[i] = static_cast<C>(upper ? wto_upper(s[i] & 0xFFFF) : wto_lower(s[i] & 0xFFFF));
  if (i == size) {
    s[0] = 0;
    return crt::kEINVAL;
  }
  return 0;
}

// --------------------------------------------------- narrow strings

SIZE_T XBEM_GUEST_ABI hle_strlen(const char* s) { return slen(s); }
SIZE_T XBEM_GUEST_ABI hle_strnlen(const char* s, SIZE_T max) { return s ? bounded_strlen(s, max) : 0; }
SIZE_T XBEM_GUEST_ABI hle___strncnt(const char* s, SIZE_T max) { return hle_strnlen(s, max); }
int32_t XBEM_GUEST_ABI hle_strcmp(const char* a, const char* b) { return scmp(a, b); }
int32_t XBEM_GUEST_ABI hle_strncmp(const char* a, const char* b, SIZE_T n) { return sncmp(a, b, n); }
int32_t XBEM_GUEST_ABI hle__stricmp(const char* a, const char* b) { return sicmp(a, b); }
int32_t XBEM_GUEST_ABI hle__strnicmp(const char* a, const char* b, SIZE_T n) { return sicmp(a, b, n); }
int32_t XBEM_GUEST_ABI hle__stricmp_l(const char* a, const char* b, void*) { return sicmp(a, b); }
int32_t XBEM_GUEST_ABI hle__strnicmp_l(const char* a, const char* b, SIZE_T n, void*) { return sicmp(a, b, n); }
int32_t XBEM_GUEST_ABI hle_strcoll(const char* a, const char* b) { return scmp(a, b); }
int32_t XBEM_GUEST_ABI hle__strcoll_l(const char* a, const char* b, void*) { return scmp(a, b); }
int32_t XBEM_GUEST_ABI hle__stricoll(const char* a, const char* b) { return sicmp(a, b); }
char* XBEM_GUEST_ABI hle_strchr(const char* s, int32_t c) { return schr(s, static_cast<uint8_t>(c)); }
char* XBEM_GUEST_ABI hle_strrchr(const char* s, int32_t c) { return srchr(s, static_cast<uint8_t>(c)); }
char* XBEM_GUEST_ABI hle_strstr(const char* h, const char* n) { return sstr(h, n); }
char* XBEM_GUEST_ABI hle_strpbrk(const char* s, const char* a) { return spbrk(s, a); }
SIZE_T XBEM_GUEST_ABI hle_strspn(const char* s, const char* a) { return sspn(s, a, false); }
SIZE_T XBEM_GUEST_ABI hle_strcspn(const char* s, const char* a) { return sspn(s, a, true); }

char* XBEM_GUEST_ABI hle_strcpy(char* d, const char* s) { return static_cast<char*>(std::memmove(d, s, slen(s) + 1)); }
char* XBEM_GUEST_ABI hle_strcat(char* d, const char* s) {
  hle_strcpy(d + slen(d), s);
  return d;
}
char* XBEM_GUEST_ABI hle_strncpy(char* d, const char* s, SIZE_T n) {
  size_t i = 0;
  for (; i < n && s[i]; ++i) d[i] = s[i];
  for (; i < n; ++i) d[i] = 0;
  return d;
}
char* XBEM_GUEST_ABI hle_strncat(char* d, const char* s, SIZE_T n) {
  char* e = d + slen(d);
  size_t i = 0;
  for (; i < n && s[i]; ++i) e[i] = s[i];
  e[i] = 0;
  return d;
}

int32_t XBEM_GUEST_ABI hle_strcpy_s(char* d, SIZE_T size, const char* s) { return copy_s(d, size, s, ~SIZE_T(0) - 1, "strcpy_s"); }
int32_t XBEM_GUEST_ABI hle_strncpy_s(char* d, SIZE_T size, const char* s, SIZE_T n) { return copy_s(d, size, s, n, "strncpy_s"); }
int32_t XBEM_GUEST_ABI hle_strcat_s(char* d, SIZE_T size, const char* s) { return cat_s(d, size, s, ~SIZE_T(0) - 1, "strcat_s"); }
int32_t XBEM_GUEST_ABI hle_strncat_s(char* d, SIZE_T size, const char* s, SIZE_T n) { return cat_s(d, size, s, n, "strncat_s"); }

thread_local char* t_strtok_context = nullptr;
thread_local wchar* t_wcstok_context = nullptr;
char* XBEM_GUEST_ABI hle_strtok(char* s, const char* d) { return stok(s, d, &t_strtok_context); }
char* XBEM_GUEST_ABI hle_strtok_s(char* s, const char* d, char** ctx) { return stok(s, d, ctx ? ctx : &t_strtok_context); }

char* XBEM_GUEST_ABI hle__strdup(const char* s) {
  if (!s) return nullptr;
  const size_t n = slen(s) + 1;
  auto* p = static_cast<char*>(heap_alloc(n, false));
  if (p) std::memcpy(p, s, n);
  return p;
}

char* XBEM_GUEST_ABI hle__strlwr(char* s) {
  for (char* p = s; *p; ++p) *p = static_cast<char>(to_lower_ascii(static_cast<uint8_t>(*p)));
  return s;
}
char* XBEM_GUEST_ABI hle__strupr(char* s) {
  for (char* p = s; *p; ++p) *p = static_cast<char>(to_upper_ascii(static_cast<uint8_t>(*p)));
  return s;
}
int32_t XBEM_GUEST_ABI hle__strlwr_s(char* s, SIZE_T n) { return case_s(s, n, false); }
int32_t XBEM_GUEST_ABI hle__strupr_s(char* s, SIZE_T n) { return case_s(s, n, true); }

SIZE_T XBEM_GUEST_ABI hle_strxfrm(char* d, const char* s, SIZE_T n) {
  const size_t len = slen(s);
  if (d && len < n) std::memcpy(d, s, len + 1);
  return len;
}

// memory
void* XBEM_GUEST_ABI hle_memcpy(void* d, const void* s, SIZE_T n) { return std::memmove(d, s, n); }
void* XBEM_GUEST_ABI hle_memmove(void* d, const void* s, SIZE_T n) { return std::memmove(d, s, n); }
void* XBEM_GUEST_ABI hle_memset(void* d, int32_t c, SIZE_T n) { return std::memset(d, c, n); }
int32_t XBEM_GUEST_ABI hle_memcmp(const void* a, const void* b, SIZE_T n) { return std::memcmp(a, b, n); }
void* XBEM_GUEST_ABI hle_memchr(const void* s, int32_t c, SIZE_T n) { return const_cast<void*>(std::memchr(s, c, n)); }
int32_t XBEM_GUEST_ABI hle__memicmp(const void* a, const void* b, SIZE_T n) {
  return sicmp(static_cast<const char*>(a), static_cast<const char*>(b), n);
}

int32_t XBEM_GUEST_ABI hle_memcpy_s(void* d, SIZE_T dsize, const void* s, SIZE_T n) {
  if (n == 0) return 0;
  if (!d) return crt::kEINVAL;
  if (!s || dsize < n) {
    std::memset(d, 0, dsize);
    crt::invalid_parameter("memcpy_s");
    return s ? crt::kERANGE : crt::kEINVAL;
  }
  std::memmove(d, s, n);
  return 0;
}

int32_t XBEM_GUEST_ABI hle_memmove_s(void* d, SIZE_T dsize, const void* s, SIZE_T n) {
  if (n == 0) return 0;
  if (!d || !s || dsize < n) {
    crt::invalid_parameter("memmove_s");
    return (!d || !s) ? crt::kEINVAL : crt::kERANGE;
  }
  std::memmove(d, s, n);
  return 0;
}

const char* error_message(int e) {
  switch (e) {
    case 0: return "No error";
    case 1: return "Operation not permitted";
    case 2: return "No such file or directory";
    case 5: return "Input/output error";
    case 9: return "Bad file descriptor";
    case 11: return "Resource temporarily unavailable";
    case 12: return "Not enough space";
    case 13: return "Permission denied";
    case 17: return "File exists";
    case 22: return "Invalid argument";
    case 24: return "Too many open files";
    case 28: return "No space left on device";
    case 33: return "Domain error";
    case 34: return "Result too large";
    case 42: return "Illegal byte sequence";
    case 80: return "String was truncated";
    default: return "Unknown error";
  }
}

char* XBEM_GUEST_ABI hle_strerror(int32_t e) { return const_cast<char*>(error_message(e)); }
int32_t XBEM_GUEST_ABI hle_strerror_s(char* buf, SIZE_T size, int32_t e) {
  return copy_s(buf, size, error_message(e), kTruncate, "strerror_s") == crt::kEINVAL ? crt::kEINVAL : 0;
}

// --------------------------------------------------------- wide strings

SIZE_T XBEM_GUEST_ABI hle_wcslen(const wchar* s) { return slen(s); }
SIZE_T XBEM_GUEST_ABI hle_wcsnlen(const wchar* s, SIZE_T max) { return s ? bounded_strlen16(s, max) : 0; }
int32_t XBEM_GUEST_ABI hle_wcscmp(const wchar* a, const wchar* b) { return scmp(a, b); }
int32_t XBEM_GUEST_ABI hle_wcsncmp(const wchar* a, const wchar* b, SIZE_T n) { return sncmp(a, b, n); }
int32_t XBEM_GUEST_ABI hle__wcsicmp(const wchar* a, const wchar* b) {
  for (;; ++a, ++b) {
    const uint32_t x = wto_lower(*a), y = wto_lower(*b);
    if (x != y) return static_cast<int32_t>(x) - static_cast<int32_t>(y);
    if (!x) return 0;
  }
}
int32_t XBEM_GUEST_ABI hle__wcsnicmp(const wchar* a, const wchar* b, SIZE_T n) {
  for (; n; --n, ++a, ++b) {
    const uint32_t x = wto_lower(*a), y = wto_lower(*b);
    if (x != y) return static_cast<int32_t>(x) - static_cast<int32_t>(y);
    if (!x) return 0;
  }
  return 0;
}
int32_t XBEM_GUEST_ABI hle__wcsicmp_l(const wchar* a, const wchar* b, void*) { return hle__wcsicmp(a, b); }
int32_t XBEM_GUEST_ABI hle__wcsnicmp_l(const wchar* a, const wchar* b, SIZE_T n, void*) { return hle__wcsnicmp(a, b, n); }
int32_t XBEM_GUEST_ABI hle_wcscoll(const wchar* a, const wchar* b) { return scmp(a, b); }
int32_t XBEM_GUEST_ABI hle__wcscoll_l(const wchar* a, const wchar* b, void*) { return scmp(a, b); }
int32_t XBEM_GUEST_ABI hle__wcsicoll(const wchar* a, const wchar* b) { return hle__wcsicmp(a, b); }
wchar* XBEM_GUEST_ABI hle_wcschr(const wchar* s, uint32_t c) { return schr(s, c & 0xFFFF); }
wchar* XBEM_GUEST_ABI hle_wcsrchr(const wchar* s, uint32_t c) { return srchr(s, c & 0xFFFF); }
wchar* XBEM_GUEST_ABI hle_wcsstr(const wchar* h, const wchar* n) { return sstr(h, n); }
wchar* XBEM_GUEST_ABI hle_wcspbrk(const wchar* s, const wchar* a) { return spbrk(s, a); }
SIZE_T XBEM_GUEST_ABI hle_wcsspn(const wchar* s, const wchar* a) { return sspn(s, a, false); }
SIZE_T XBEM_GUEST_ABI hle_wcscspn(const wchar* s, const wchar* a) { return sspn(s, a, true); }
wchar* XBEM_GUEST_ABI hle_wcscpy(wchar* d, const wchar* s) {
  return static_cast<wchar*>(std::memmove(d, s, (slen(s) + 1) * 2));
}
wchar* XBEM_GUEST_ABI hle_wcscat(wchar* d, const wchar* s) {
  hle_wcscpy(d + slen(d), s);
  return d;
}
wchar* XBEM_GUEST_ABI hle_wcsncpy(wchar* d, const wchar* s, SIZE_T n) {
  size_t i = 0;
  for (; i < n && s[i]; ++i) d[i] = s[i];
  for (; i < n; ++i) d[i] = 0;
  return d;
}
wchar* XBEM_GUEST_ABI hle_wcsncat(wchar* d, const wchar* s, SIZE_T n) {
  wchar* e = d + slen(d);
  size_t i = 0;
  for (; i < n && s[i]; ++i) e[i] = s[i];
  e[i] = 0;
  return d;
}
int32_t XBEM_GUEST_ABI hle_wcscpy_s(wchar* d, SIZE_T size, const wchar* s) { return copy_s(d, size, s, ~SIZE_T(0) - 1, "wcscpy_s"); }
int32_t XBEM_GUEST_ABI hle_wcsncpy_s(wchar* d, SIZE_T size, const wchar* s, SIZE_T n) { return copy_s(d, size, s, n, "wcsncpy_s"); }
int32_t XBEM_GUEST_ABI hle_wcscat_s(wchar* d, SIZE_T size, const wchar* s) { return cat_s(d, size, s, ~SIZE_T(0) - 1, "wcscat_s"); }
int32_t XBEM_GUEST_ABI hle_wcsncat_s(wchar* d, SIZE_T size, const wchar* s, SIZE_T n) { return cat_s(d, size, s, n, "wcsncat_s"); }
wchar* XBEM_GUEST_ABI hle_wcstok(wchar* s, const wchar* d, wchar** ctx) { return stok(s, d, ctx ? ctx : &t_wcstok_context); }
wchar* XBEM_GUEST_ABI hle_wcstok_s(wchar* s, const wchar* d, wchar** ctx) { return stok(s, d, ctx ? ctx : &t_wcstok_context); }
wchar* XBEM_GUEST_ABI hle__wcstok(wchar* s, const wchar* d) { return stok(s, d, &t_wcstok_context); }

wchar* XBEM_GUEST_ABI hle__wcsdup(const wchar* s) {
  if (!s) return nullptr;
  const size_t n = (slen(s) + 1) * 2;
  auto* p = static_cast<wchar*>(heap_alloc(n, false));
  if (p) std::memcpy(p, s, n);
  return p;
}

wchar* XBEM_GUEST_ABI hle__wcslwr(wchar* s) {
  for (wchar* p = s; *p; ++p) *p = static_cast<wchar>(wto_lower(*p));
  return s;
}
wchar* XBEM_GUEST_ABI hle__wcsupr(wchar* s) {
  for (wchar* p = s; *p; ++p) *p = static_cast<wchar>(wto_upper(*p));
  return s;
}
int32_t XBEM_GUEST_ABI hle__wcslwr_s(wchar* s, SIZE_T n) { return case_s(s, n, false); }
int32_t XBEM_GUEST_ABI hle__wcsupr_s(wchar* s, SIZE_T n) { return case_s(s, n, true); }

SIZE_T XBEM_GUEST_ABI hle_wcsxfrm(wchar* d, const wchar* s, SIZE_T n) {
  const size_t len = slen(s);
  if (d && len < n) std::memcpy(d, s, (len + 1) * 2);
  return len;
}

wchar* XBEM_GUEST_ABI hle_wmemcpy(wchar* d, const wchar* s, SIZE_T n) { return static_cast<wchar*>(std::memmove(d, s, n * 2)); }
wchar* XBEM_GUEST_ABI hle_wmemmove(wchar* d, const wchar* s, SIZE_T n) { return static_cast<wchar*>(std::memmove(d, s, n * 2)); }
wchar* XBEM_GUEST_ABI hle_wmemset(wchar* d, uint32_t c, SIZE_T n) {
  for (SIZE_T i = 0; i < n; ++i) d[i] = static_cast<wchar>(c);
  return d;
}
int32_t XBEM_GUEST_ABI hle_wmemcmp(const wchar* a, const wchar* b, SIZE_T n) {
  for (SIZE_T i = 0; i < n; ++i)
    if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
  return 0;
}
wchar* XBEM_GUEST_ABI hle_wmemchr(const wchar* s, uint32_t c, SIZE_T n) {
  for (SIZE_T i = 0; i < n; ++i)
    if (s[i] == static_cast<wchar>(c)) return const_cast<wchar*>(s + i);
  return nullptr;
}

// ------------------------------------------------------- number parsing

// Parses with the host strto* on an ASCII view of the guest string so that
// character positions map 1:1 for the end pointer.
template <class C>
std::string ascii_view(const C* s) {
  std::string out;
  for (; *s; ++s) out.push_back(static_cast<uint32_t>(*s) < 0x80 ? static_cast<char>(*s) : '\x01');
  return out;
}

template <class C, class Fn>
auto parse_number(const C* s, C** end, Fn&& parse) {
  const std::string a = ascii_view(s);
  char* host_end = nullptr;
  errno = 0;
  auto v = parse(a.c_str(), &host_end);
  if (errno == ERANGE) set_errno(crt::kERANGE);
  if (end) *end = const_cast<C*>(s + (host_end - a.c_str()));
  return v;
}

template <class C>
int32_t parse_long(const C* s, C** end, int base) {
  const long long v = parse_number(s, end, [&](const char* p, char** e) { return std::strtoll(p, e, base); });
  if (v > std::numeric_limits<int32_t>::max()) {
    set_errno(crt::kERANGE);
    return std::numeric_limits<int32_t>::max();
  }
  if (v < std::numeric_limits<int32_t>::min()) {
    set_errno(crt::kERANGE);
    return std::numeric_limits<int32_t>::min();
  }
  return static_cast<int32_t>(v);
}

template <class C>
uint32_t parse_ulong(const C* s, C** end, int base) {
  bool range = false;
  const unsigned long long v = parse_number(s, end, [&](const char* p, char** e) {
    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) ++p;
    const bool negative = *p == '-';
    errno = 0;
    unsigned long long r = std::strtoull(p, e, base);
    if (errno == ERANGE) range = true;
    if (negative) {
      const unsigned long long magnitude = ~r + 1;
      if (magnitude > 0xFFFFFFFFull) range = true;
      r = static_cast<uint32_t>(r);
    } else if (r > 0xFFFFFFFFull) {
      range = true;
    }
    return r;
  });
  if (range) {
    set_errno(crt::kERANGE);
    return 0xFFFFFFFFu;
  }
  return static_cast<uint32_t>(v);
}

int32_t XBEM_GUEST_ABI hle_strtol(const char* s, char** e, int32_t b) { return parse_long(s, e, b); }
uint32_t XBEM_GUEST_ABI hle_strtoul(const char* s, char** e, int32_t b) { return parse_ulong(s, e, b); }
int64_t XBEM_GUEST_ABI hle_strtoll(const char* s, char** e, int32_t b) {
  return parse_number(s, e, [&](const char* p, char** x) { return std::strtoll(p, x, b); });
}
uint64_t XBEM_GUEST_ABI hle_strtoull(const char* s, char** e, int32_t b) {
  return parse_number(s, e, [&](const char* p, char** x) { return std::strtoull(p, x, b); });
}
double XBEM_GUEST_ABI hle_strtod(const char* s, char** e) {
  return parse_number(s, e, [](const char* p, char** x) { return std::strtod(p, x); });
}
float XBEM_GUEST_ABI hle_strtof(const char* s, char** e) {
  return parse_number(s, e, [](const char* p, char** x) { return std::strtof(p, x); });
}
int32_t XBEM_GUEST_ABI hle_strtol_l(const char* s, char** e, int32_t b, void*) { return hle_strtol(s, e, b); }
double XBEM_GUEST_ABI hle__strtod_l(const char* s, char** e, void*) { return hle_strtod(s, e); }
int32_t XBEM_GUEST_ABI hle_atoi(const char* s) { return static_cast<int32_t>(hle_strtoll(s, nullptr, 10)); }
int32_t XBEM_GUEST_ABI hle_atol(const char* s) { return hle_atoi(s); }
int64_t XBEM_GUEST_ABI hle_atoll(const char* s) { return hle_strtoll(s, nullptr, 10); }
double XBEM_GUEST_ABI hle_atof(const char* s) { return hle_strtod(s, nullptr); }

int32_t XBEM_GUEST_ABI hle_wcstol(const wchar* s, wchar** e, int32_t b) { return parse_long(s, e, b); }
uint32_t XBEM_GUEST_ABI hle_wcstoul(const wchar* s, wchar** e, int32_t b) { return parse_ulong(s, e, b); }
int64_t XBEM_GUEST_ABI hle_wcstoll(const wchar* s, wchar** e, int32_t b) {
  return parse_number(s, e, [&](const char* p, char** x) { return std::strtoll(p, x, b); });
}
uint64_t XBEM_GUEST_ABI hle_wcstoull(const wchar* s, wchar** e, int32_t b) {
  return parse_number(s, e, [&](const char* p, char** x) { return std::strtoull(p, x, b); });
}
double XBEM_GUEST_ABI hle_wcstod(const wchar* s, wchar** e) {
  return parse_number(s, e, [](const char* p, char** x) { return std::strtod(p, x); });
}
float XBEM_GUEST_ABI hle_wcstof(const wchar* s, wchar** e) {
  return parse_number(s, e, [](const char* p, char** x) { return std::strtof(p, x); });
}
int32_t XBEM_GUEST_ABI hle__wtoi(const wchar* s) { return static_cast<int32_t>(hle_wcstoll(s, nullptr, 10)); }
int64_t XBEM_GUEST_ABI hle__wtoll(const wchar* s) { return hle_wcstoll(s, nullptr, 10); }
double XBEM_GUEST_ABI hle__wtof(const wchar* s) { return hle_wcstod(s, nullptr); }

// ------------------------------------------------------- number to text

template <class C>
int32_t to_text_s(uint64_t magnitude, bool negative, C* buf, SIZE_T size, int32_t radix) {
  if (!buf || size == 0 || radix < 2 || radix > 36) return crt::kEINVAL;
  char tmp[72];
  size_t n = 0;
  do {
    const unsigned d = static_cast<unsigned>(magnitude % static_cast<unsigned>(radix));
    tmp[n++] = static_cast<char>(d < 10 ? '0' + d : 'a' + d - 10);
    magnitude /= static_cast<unsigned>(radix);
  } while (magnitude);
  if (negative) tmp[n++] = '-';
  if (n + 1 > size) {
    buf[0] = 0;
    crt::invalid_parameter("_itoa_s");
    return crt::kERANGE;
  }
  for (size_t i = 0; i < n; ++i) buf[i] = static_cast<C>(tmp[n - 1 - i]);
  buf[n] = 0;
  return 0;
}

template <class C>
int32_t signed_s(int64_t v, C* buf, SIZE_T size, int32_t radix) {
  const bool neg = radix == 10 && v < 0;
  const uint64_t mag = neg ? ~static_cast<uint64_t>(v) + 1 : static_cast<uint64_t>(v);
  return to_text_s(mag, neg, buf, size, radix);
}

int32_t XBEM_GUEST_ABI hle__itoa_s(int32_t v, char* b, SIZE_T n, int32_t r) {
  return r == 10 ? signed_s<char>(v, b, n, r) : to_text_s<char>(static_cast<uint32_t>(v), false, b, n, r);
}
int32_t XBEM_GUEST_ABI hle__ltoa_s(int32_t v, char* b, SIZE_T n, int32_t r) { return hle__itoa_s(v, b, n, r); }
int32_t XBEM_GUEST_ABI hle__ultoa_s(uint32_t v, char* b, SIZE_T n, int32_t r) { return to_text_s<char>(v, false, b, n, r); }
int32_t XBEM_GUEST_ABI hle__i64toa_s(int64_t v, char* b, SIZE_T n, int32_t r) { return signed_s<char>(v, b, n, r); }
int32_t XBEM_GUEST_ABI hle__ui64toa_s(uint64_t v, char* b, SIZE_T n, int32_t r) { return to_text_s<char>(v, false, b, n, r); }
int32_t XBEM_GUEST_ABI hle__itow_s(int32_t v, wchar* b, SIZE_T n, int32_t r) {
  return r == 10 ? signed_s<wchar>(v, b, n, r) : to_text_s<wchar>(static_cast<uint32_t>(v), false, b, n, r);
}
int32_t XBEM_GUEST_ABI hle__ltow_s(int32_t v, wchar* b, SIZE_T n, int32_t r) { return hle__itow_s(v, b, n, r); }
int32_t XBEM_GUEST_ABI hle__ultow_s(uint32_t v, wchar* b, SIZE_T n, int32_t r) { return to_text_s<wchar>(v, false, b, n, r); }
int32_t XBEM_GUEST_ABI hle__i64tow_s(int64_t v, wchar* b, SIZE_T n, int32_t r) { return signed_s<wchar>(v, b, n, r); }
int32_t XBEM_GUEST_ABI hle__ui64tow_s(uint64_t v, wchar* b, SIZE_T n, int32_t r) { return to_text_s<wchar>(v, false, b, n, r); }
char* XBEM_GUEST_ABI hle__itoa(int32_t v, char* b, int32_t r) {
  hle__itoa_s(v, b, 34, r);
  return b;
}
char* XBEM_GUEST_ABI hle__i64toa(int64_t v, char* b, int32_t r) {
  hle__i64toa_s(v, b, 66, r);
  return b;
}
char* XBEM_GUEST_ABI hle__ui64toa(uint64_t v, char* b, int32_t r) {
  hle__ui64toa_s(v, b, 66, r);
  return b;
}
wchar* XBEM_GUEST_ABI hle__itow(int32_t v, wchar* b, int32_t r) {
  hle__itow_s(v, b, 34, r);
  return b;
}

// ------------------------------------------- multibyte ("C" locale: 1:1)

int32_t XBEM_GUEST_ABI hle_mbtowc(wchar* out, const char* s, SIZE_T n) {
  if (!s) return 0;
  if (n == 0) return -1;
  if (out) *out = static_cast<uint8_t>(*s);
  return *s ? 1 : 0;
}

int32_t XBEM_GUEST_ABI hle_wctomb(char* s, uint32_t wc) {
  if (!s) return 0;
  if ((wc & 0xFFFF) > 0xFF) {
    set_errno(crt::kEILSEQ);
    return -1;
  }
  *s = static_cast<char>(wc);
  return 1;
}

int32_t XBEM_GUEST_ABI hle_wctomb_s(int32_t* written, char* s, SIZE_T size, uint32_t wc) {
  if (!s) {
    if (written) *written = 0;
    return 0;
  }
  if (size < 1) return crt::kERANGE;
  const int32_t r = hle_wctomb(s, wc);
  if (written) *written = r;
  return r < 0 ? crt::kEILSEQ : 0;
}

SIZE_T XBEM_GUEST_ABI hle_mbrtowc(wchar* out, const char* s, SIZE_T n, void*) {
  if (!s) return 0;
  if (n == 0) return ~SIZE_T(0) - 1;  // (size_t)-2: incomplete
  if (out) *out = static_cast<uint8_t>(*s);
  return *s ? 1 : 0;
}

SIZE_T XBEM_GUEST_ABI hle_wcrtomb(char* s, uint32_t wc, void*) {
  char tmp;
  const int32_t r = hle_wctomb(s ? s : &tmp, wc);
  return r < 0 ? ~SIZE_T(0) : static_cast<SIZE_T>(r);
}

int32_t XBEM_GUEST_ABI hle_wcrtomb_s(SIZE_T* written, char* s, SIZE_T size, uint32_t wc, void*) {
  int32_t w = 0;
  const int32_t r = hle_wctomb_s(&w, s, size, wc);
  if (written) *written = w < 0 ? ~SIZE_T(0) : static_cast<SIZE_T>(w);
  return r;
}

int32_t XBEM_GUEST_ABI hle_mblen(const char* s, SIZE_T n) { return (!s || n == 0) ? 0 : (*s ? 1 : 0); }
SIZE_T XBEM_GUEST_ABI hle_mbrlen(const char* s, SIZE_T n, void*) { return (!s || n == 0) ? 0 : (*s ? 1 : 0); }
uint32_t XBEM_GUEST_ABI hle_btowc(int32_t c) { return c == -1 ? 0xFFFF : static_cast<uint8_t>(c); }
int32_t XBEM_GUEST_ABI hle_wctob(uint32_t wc) { return (wc & 0xFFFF) <= 0xFF ? static_cast<int32_t>(wc & 0xFF) : -1; }
int32_t XBEM_GUEST_ABI hle_mbsinit(const void*) { return 1; }

SIZE_T XBEM_GUEST_ABI hle_mbstowcs(wchar* out, const char* s, SIZE_T n) {
  const size_t len = slen(s);
  if (!out) return len;
  size_t i = 0;
  for (; i < n && i < len; ++i) out[i] = static_cast<uint8_t>(s[i]);
  if (i < n) out[i] = 0;
  return i;
}

SIZE_T XBEM_GUEST_ABI hle_wcstombs(char* out, const wchar* s, SIZE_T n) {
  const size_t len = slen(s);
  for (size_t i = 0; i < len; ++i)
    if (s[i] > 0xFF) {
      set_errno(crt::kEILSEQ);
      return ~SIZE_T(0);
    }
  if (!out) return len;
  size_t i = 0;
  for (; i < n && i < len; ++i) out[i] = static_cast<char>(s[i]);
  if (i < n) out[i] = 0;
  return i;
}

int32_t XBEM_GUEST_ABI hle_mbstowcs_s(SIZE_T* converted, wchar* out, SIZE_T size, const char* s, SIZE_T count) {
  if (!s) return crt::kEINVAL;
  size_t len = slen(s);
  if (count != kTruncate) len = std::min<size_t>(len, count);
  if (!out) {
    if (converted) *converted = len + 1;
    return 0;
  }
  if (len + 1 > size) {
    if (count != kTruncate) {
      if (size) out[0] = 0;
      return crt::kERANGE;
    }
    len = size - 1;
  }
  for (size_t i = 0; i < len; ++i) out[i] = static_cast<uint8_t>(s[i]);
  out[len] = 0;
  if (converted) *converted = len + 1;
  return 0;
}

int32_t XBEM_GUEST_ABI hle_wcstombs_s(SIZE_T* converted, char* out, SIZE_T size, const wchar* s, SIZE_T count) {
  if (!s) return crt::kEINVAL;
  size_t len = slen(s);
  if (count != kTruncate) len = std::min<size_t>(len, count);
  for (size_t i = 0; i < len; ++i)
    if (s[i] > 0xFF) {
      if (out && size) out[0] = 0;
      return crt::kEILSEQ;
    }
  if (!out) {
    if (converted) *converted = len + 1;
    return 0;
  }
  if (len + 1 > size) {
    if (count != kTruncate) {
      if (size) out[0] = 0;
      return crt::kERANGE;
    }
    len = size - 1;
  }
  for (size_t i = 0; i < len; ++i) out[i] = static_cast<char>(s[i]);
  out[len] = 0;
  if (converted) *converted = len + 1;
  return 0;
}

// ---------------------------------------------------------------- locale

// struct lconv (MSVC layout: 8 narrow strings, 8 chars, 8 wide strings).
struct Lconv {
  const char* decimal_point;
  const char* thousands_sep;
  const char* grouping;
  const char* int_curr_symbol;
  const char* currency_symbol;
  const char* mon_decimal_point;
  const char* mon_thousands_sep;
  const char* mon_grouping;
  const char* positive_sign;
  const char* negative_sign;
  char int_frac_digits, frac_digits, p_cs_precedes, p_sep_by_space, n_cs_precedes, n_sep_by_space, p_sign_posn,
      n_sign_posn;
  const char16_t* w_decimal_point;
  const char16_t* w_thousands_sep;
  const char16_t* w_int_curr_symbol;
  const char16_t* w_currency_symbol;
  const char16_t* w_mon_decimal_point;
  const char16_t* w_mon_thousands_sep;
  const char16_t* w_positive_sign;
  const char16_t* w_negative_sign;
};

Lconv g_lconv = {".", "", "", "", "", "", "", "", "", "", 127, 127, 127, 127, 127, 127, 127, 127,
                 u".", u"", u"", u"", u"", u"", u"", u""};

char g_locale_name[] = "C";
char16_t g_wlocale_name[] = u"C";

char* XBEM_GUEST_ABI hle_setlocale(int32_t, const char* name) {
  if (name && *name && std::strcmp(name, "C") != 0)
    XBEM_DEBUG("ucrt", "setlocale(\"{}\") -> staying in the \"C\" locale", name);
  return g_locale_name;
}
char16_t* XBEM_GUEST_ABI hle__wsetlocale(int32_t, const char16_t*) { return g_wlocale_name; }
Lconv* XBEM_GUEST_ABI hle_localeconv() { return &g_lconv; }

// _locale_t points at two pointers (locinfo, mbcinfo); we hand out a dummy.
uint64_t g_dummy_locale[4] = {};
void* XBEM_GUEST_ABI hle__create_locale(int32_t, const char*) { return g_dummy_locale; }
void* XBEM_GUEST_ABI hle__wcreate_locale(int32_t, const char16_t*) { return g_dummy_locale; }
void XBEM_GUEST_ABI hle__free_locale(void*) {}
void* XBEM_GUEST_ABI hle__get_current_locale() { return g_dummy_locale; }
int32_t XBEM_GUEST_ABI hle__configthreadlocale(int32_t) { return 2; }  // _DISABLE_PER_THREAD_LOCALE
UINT XBEM_GUEST_ABI hle____lc_codepage_func() { return 0; }
UINT XBEM_GUEST_ABI hle____lc_collate_cp_func() { return 0; }
char16_t* g_locale_names[6] = {};
char16_t** XBEM_GUEST_ABI hle____lc_locale_name_func() { return g_locale_names; }
int32_t XBEM_GUEST_ABI hle____mb_cur_max_func() { return 1; }
int32_t XBEM_GUEST_ABI hle____mb_cur_max_l_func(void*) { return 1; }
void XBEM_GUEST_ABI hle__lock_locales() {}
void XBEM_GUEST_ABI hle__unlock_locales() {}

}  // namespace

void add_ucrt_string_exports(std::vector<ExportDef>& out) {
  out.insert(out.end(), {
      XBEM_HLE(__strncnt), XBEM_HLE(isalpha), XBEM_HLE(isupper), XBEM_HLE(islower), XBEM_HLE(isdigit), XBEM_HLE(isxdigit),
      XBEM_HLE(isspace), XBEM_HLE(ispunct), XBEM_HLE(isalnum), XBEM_HLE(isgraph), XBEM_HLE(iscntrl),
      XBEM_HLE(isblank), XBEM_HLE(_isalpha_l), XBEM_HLE(_isupper_l), XBEM_HLE(_islower_l), XBEM_HLE(_isdigit_l),
      XBEM_HLE(_isxdigit_l), XBEM_HLE(_isspace_l), XBEM_HLE(_ispunct_l), XBEM_HLE(_isalnum_l), XBEM_HLE(_isgraph_l),
      XBEM_HLE(_iscntrl_l), XBEM_HLE(_isblank_l), XBEM_HLE(_isprint_l),
      ExportDef{"isprint", reinterpret_cast<void*>(&hle_isprint_fix)}, XBEM_HLE(toupper), XBEM_HLE(tolower),
      XBEM_HLE(_toupper_l), XBEM_HLE(_tolower_l), XBEM_HLE(_toupper), XBEM_HLE(_tolower), XBEM_HLE(__isascii),
      XBEM_HLE(__toascii), XBEM_HLE(__iscsym), XBEM_HLE(__iscsymf), XBEM_HLE(_isctype), XBEM_HLE(_isctype_l),
      XBEM_HLE(__pctype_func), XBEM_HLE(__pwctype_func), XBEM_HLE(iswalpha), XBEM_HLE(iswupper), XBEM_HLE(iswlower),
      XBEM_HLE(iswdigit), XBEM_HLE(iswxdigit), XBEM_HLE(iswspace), XBEM_HLE(iswpunct), XBEM_HLE(iswalnum),
      XBEM_HLE(iswprint), XBEM_HLE(iswgraph), XBEM_HLE(iswcntrl), XBEM_HLE(iswblank), XBEM_HLE(iswascii),
      XBEM_HLE(iswctype), XBEM_HLE(_iswctype_l), XBEM_HLE(towupper), XBEM_HLE(towlower), XBEM_HLE(_towupper_l),
      XBEM_HLE(_towlower_l), XBEM_HLE(isleadbyte), XBEM_HLE(_isleadbyte_l), XBEM_HLE(strlen), XBEM_HLE(strnlen),
      XBEM_HLE(strcmp), XBEM_HLE(strncmp), XBEM_HLE(_stricmp), XBEM_HLE(_strnicmp), XBEM_HLE(_stricmp_l),
      XBEM_HLE(_strnicmp_l), XBEM_HLE(strcoll), XBEM_HLE(_strcoll_l), XBEM_HLE(_stricoll), XBEM_HLE(strchr),
      XBEM_HLE(strrchr), XBEM_HLE(strstr), XBEM_HLE(strpbrk), XBEM_HLE(strspn), XBEM_HLE(strcspn), XBEM_HLE(strcpy),
      XBEM_HLE(strcat), XBEM_HLE(strncpy), XBEM_HLE(strncat), XBEM_HLE(strcpy_s), XBEM_HLE(strncpy_s),
      XBEM_HLE(strcat_s), XBEM_HLE(strncat_s), XBEM_HLE(strtok), XBEM_HLE(strtok_s), XBEM_HLE(_strdup),
      XBEM_HLE(_strlwr), XBEM_HLE(_strupr), XBEM_HLE(_strlwr_s), XBEM_HLE(_strupr_s), XBEM_HLE(strxfrm),
      XBEM_HLE(memcpy), XBEM_HLE(memmove), XBEM_HLE(memset), XBEM_HLE(memcmp), XBEM_HLE(memchr), XBEM_HLE(_memicmp),
      XBEM_HLE(memcpy_s), XBEM_HLE(memmove_s), XBEM_HLE(strerror), XBEM_HLE(strerror_s), XBEM_HLE(wcslen),
      XBEM_HLE(wcsnlen), XBEM_HLE(wcscmp), XBEM_HLE(wcsncmp), XBEM_HLE(_wcsicmp), XBEM_HLE(_wcsnicmp),
      XBEM_HLE(_wcsicmp_l), XBEM_HLE(_wcsnicmp_l), XBEM_HLE(wcscoll), XBEM_HLE(_wcscoll_l), XBEM_HLE(_wcsicoll),
      XBEM_HLE(wcschr), XBEM_HLE(wcsrchr), XBEM_HLE(wcsstr), XBEM_HLE(wcspbrk), XBEM_HLE(wcsspn), XBEM_HLE(wcscspn),
      XBEM_HLE(wcscpy), XBEM_HLE(wcscat), XBEM_HLE(wcsncpy), XBEM_HLE(wcsncat), XBEM_HLE(wcscpy_s),
      XBEM_HLE(wcsncpy_s), XBEM_HLE(wcscat_s), XBEM_HLE(wcsncat_s), XBEM_HLE(wcstok), XBEM_HLE(wcstok_s),
      XBEM_HLE(_wcstok), XBEM_HLE(_wcsdup), XBEM_HLE(_wcslwr), XBEM_HLE(_wcsupr), XBEM_HLE(_wcslwr_s),
      XBEM_HLE(_wcsupr_s), XBEM_HLE(wcsxfrm), XBEM_HLE(wmemcpy), XBEM_HLE(wmemmove), XBEM_HLE(wmemset),
      XBEM_HLE(wmemcmp), XBEM_HLE(wmemchr), XBEM_HLE(strtol), XBEM_HLE(strtoul), XBEM_HLE(strtoll),
      XBEM_HLE(strtoull), XBEM_HLE(strtod), XBEM_HLE(strtof), XBEM_HLE(_strtod_l), XBEM_HLE(atoi), XBEM_HLE(atol),
      XBEM_HLE(atoll), XBEM_HLE(atof), XBEM_HLE(wcstol), XBEM_HLE(wcstoul), XBEM_HLE(wcstoll), XBEM_HLE(wcstoull),
      XBEM_HLE(wcstod), XBEM_HLE(wcstof), XBEM_HLE(_wtoi), XBEM_HLE(_wtoll), XBEM_HLE(_wtof),
      ExportDef{"_strtol_l", reinterpret_cast<void*>(&hle_strtol_l)},
      ExportDef{"_strtoi64", reinterpret_cast<void*>(&hle_strtoll)},
      ExportDef{"_strtoui64", reinterpret_cast<void*>(&hle_strtoull)},
      ExportDef{"_atoi64", reinterpret_cast<void*>(&hle_atoll)},
      ExportDef{"_wcstoi64", reinterpret_cast<void*>(&hle_wcstoll)},
      ExportDef{"_wcstoui64", reinterpret_cast<void*>(&hle_wcstoull)},
      ExportDef{"_wtol", reinterpret_cast<void*>(&hle__wtoi)},
      ExportDef{"_wtoi64", reinterpret_cast<void*>(&hle__wtoll)}, XBEM_HLE(_itoa_s), XBEM_HLE(_ltoa_s),
      XBEM_HLE(_ultoa_s), XBEM_HLE(_i64toa_s), XBEM_HLE(_ui64toa_s), XBEM_HLE(_itow_s), XBEM_HLE(_ltow_s),
      XBEM_HLE(_ultow_s), XBEM_HLE(_i64tow_s), XBEM_HLE(_ui64tow_s), XBEM_HLE(_itoa), XBEM_HLE(_i64toa),
      XBEM_HLE(_ui64toa), XBEM_HLE(_itow), XBEM_HLE(mbtowc), XBEM_HLE(wctomb), XBEM_HLE(wctomb_s), XBEM_HLE(mbrtowc),
      XBEM_HLE(wcrtomb), XBEM_HLE(wcrtomb_s), XBEM_HLE(mblen), XBEM_HLE(mbrlen), XBEM_HLE(btowc), XBEM_HLE(wctob),
      XBEM_HLE(mbsinit), XBEM_HLE(mbstowcs), XBEM_HLE(wcstombs), XBEM_HLE(mbstowcs_s), XBEM_HLE(wcstombs_s),
      XBEM_HLE(setlocale), XBEM_HLE(_wsetlocale), XBEM_HLE(localeconv), XBEM_HLE(_create_locale),
      XBEM_HLE(_wcreate_locale), XBEM_HLE(_free_locale), XBEM_HLE(_get_current_locale),
      XBEM_HLE(_configthreadlocale), XBEM_HLE(___lc_codepage_func), XBEM_HLE(___lc_collate_cp_func),
      XBEM_HLE(___lc_locale_name_func), XBEM_HLE(___mb_cur_max_func), XBEM_HLE(___mb_cur_max_l_func),
      XBEM_HLE(_lock_locales), XBEM_HLE(_unlock_locales),
  });
}

}  // namespace xbem::hle
