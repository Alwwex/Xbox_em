// UCRT: time, sorting/searching, random numbers, bit helpers, the
// environment and file-system functions (_stat, _findfirst, _access, ...).

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <map>
#include <mutex>
#include <numeric>
#include <random>

#include "hle/heap.h"
#include "hle/ucrt.h"

namespace xbem::hle {
namespace {

using namespace win;
using crt::set_errno;
namespace fs = std::filesystem;

// ----------------------------------------------------------------- time

struct GuestTm {
  int32_t sec, min, hour, mday, mon, year, wday, yday, isdst;
};

std::tm to_host(const GuestTm& g) {
  std::tm t{};
  t.tm_sec = g.sec;
  t.tm_min = g.min;
  t.tm_hour = g.hour;
  t.tm_mday = g.mday;
  t.tm_mon = g.mon;
  t.tm_year = g.year;
  t.tm_wday = g.wday;
  t.tm_yday = g.yday;
  t.tm_isdst = g.isdst;
  return t;
}

GuestTm from_host(const std::tm& t) {
  return {t.tm_sec, t.tm_min, t.tm_hour, t.tm_mday, t.tm_mon, t.tm_year, t.tm_wday, t.tm_yday, t.tm_isdst};
}

bool host_time(int64_t t, bool local, std::tm& out) {
  const std::time_t tt = static_cast<std::time_t>(t);
#if defined(_WIN32)
  return (local ? localtime_s(&out, &tt) : gmtime_s(&out, &tt)) == 0;
#else
  return (local ? localtime_r(&tt, &out) : gmtime_r(&tt, &out)) != nullptr;
#endif
}

int64_t XBEM_GUEST_ABI hle__time64(int64_t* out) {
  const int64_t t = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
  if (out) *out = t;
  return t;
}

int32_t XBEM_GUEST_ABI hle__time32(int32_t* out) {
  const auto t = static_cast<int32_t>(hle__time64(nullptr));
  if (out) *out = t;
  return t;
}

int32_t convert_time_s(GuestTm* out, const int64_t* t, bool local) {
  if (!out || !t) return crt::kEINVAL;
  std::tm tm{};
  if (*t < 0 || !host_time(*t, local, tm)) {
    std::memset(out, 0xFF, sizeof *out);
    return crt::kEINVAL;
  }
  *out = from_host(tm);
  return 0;
}

int32_t XBEM_GUEST_ABI hle__gmtime64_s(GuestTm* out, const int64_t* t) { return convert_time_s(out, t, false); }
int32_t XBEM_GUEST_ABI hle__localtime64_s(GuestTm* out, const int64_t* t) { return convert_time_s(out, t, true); }

thread_local GuestTm t_tm;
GuestTm* XBEM_GUEST_ABI hle__gmtime64(const int64_t* t) { return convert_time_s(&t_tm, t, false) == 0 ? &t_tm : nullptr; }
GuestTm* XBEM_GUEST_ABI hle__localtime64(const int64_t* t) { return convert_time_s(&t_tm, t, true) == 0 ? &t_tm : nullptr; }

int64_t XBEM_GUEST_ABI hle__mktime64(GuestTm* g) {
  if (!g) return -1;
  std::tm t = to_host(*g);
  const std::time_t r = std::mktime(&t);
  if (r != static_cast<std::time_t>(-1)) *g = from_host(t);
  return static_cast<int64_t>(r);
}

int64_t XBEM_GUEST_ABI hle__mkgmtime64(GuestTm* g) {
  if (!g) return -1;
  std::tm t = to_host(*g);
#if defined(_WIN32)
  const std::time_t r = _mkgmtime(&t);
#else
  const std::time_t r = timegm(&t);
#endif
  if (r != static_cast<std::time_t>(-1)) *g = from_host(t);
  return static_cast<int64_t>(r);
}

double XBEM_GUEST_ABI hle__difftime64(int64_t a, int64_t b) { return static_cast<double>(a - b); }

const auto g_clock_start = std::chrono::steady_clock::now();
int32_t XBEM_GUEST_ABI hle_clock() {  // CLOCKS_PER_SEC is 1000 on Windows
  return static_cast<int32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_clock_start).count());
}

SIZE_T XBEM_GUEST_ABI hle_strftime(char* buf, SIZE_T size, const char* fmt, const GuestTm* g) {
  if (!buf || !fmt || !g || size == 0) return 0;
  const std::tm t = to_host(*g);
  return std::strftime(buf, size, fmt, &t);
}

SIZE_T XBEM_GUEST_ABI hle_wcsftime(char16_t* buf, SIZE_T size, const char16_t* fmt, const GuestTm* g) {
  if (!buf || !fmt || !g || size == 0) return 0;
  const std::tm t = to_host(*g);
  char tmp[1024];
  const size_t n = std::strftime(tmp, sizeof tmp, narrow(fmt).c_str(), &t);
  const std::u16string w = crt::to_wide(std::string_view(tmp, n));
  if (w.size() + 1 > size) return 0;
  std::memcpy(buf, w.data(), w.size() * 2);
  buf[w.size()] = 0;
  return w.size();
}

void XBEM_GUEST_ABI hle__tzset() {}
int32_t XBEM_GUEST_ABI hle__get_timezone(int32_t* s) {
  if (s) *s = 0;
  return 0;
}
int32_t XBEM_GUEST_ABI hle__get_daylight(int32_t* d) {
  if (d) *d = 0;
  return 0;
}
int32_t XBEM_GUEST_ABI hle__get_dstbias(int32_t* b) {
  if (b) *b = 0;
  return 0;
}

struct Timespec64 {
  int64_t sec;
  int32_t nsec;
  int32_t pad;
};

int32_t XBEM_GUEST_ABI hle__timespec64_get(Timespec64* ts, int32_t base) {
  if (!ts || base != 1) return 0;  // TIME_UTC
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
  ts->sec = ns / 1000000000;
  ts->nsec = static_cast<int32_t>(ns % 1000000000);
  return base;
}

struct Timeb64 {
  int64_t time;
  uint16_t millitm;
  int16_t timezone;
  int16_t dstflag;
};

int32_t XBEM_GUEST_ABI hle__ftime64_s(Timeb64* tb) {
  if (!tb) return crt::kEINVAL;
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
  tb->time = ms / 1000;
  tb->millitm = static_cast<uint16_t>(ms % 1000);
  tb->timezone = 0;
  tb->dstflag = 0;
  return 0;
}

// ------------------------------------------------------- sort & search

void XBEM_GUEST_ABI hle__ftime64(Timeb64* tb) { hle__ftime64_s(tb); }

// Locale time names used by msvcp140 (time_get/time_put). The C locale.
constexpr const char* kCDayAbbr[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
constexpr const char* kCDay[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
constexpr const char* kCMonAbbr[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
constexpr const char* kCMon[12] = {"January", "February", "March",     "April",   "May",      "June",
                                   "July",    "August",   "September", "October", "November", "December"};

std::string joined_names(const char* const* abbr, const char* const* full, int n) {
  std::string s;
  for (int i = 0; i < n; ++i) s += std::string(":") + abbr[i] + ":" + full[i];
  return s;
}

template <class Char>
Char* heap_string(const std::basic_string<Char>& s) {
  auto* p = static_cast<Char*>(heap_alloc((s.size() + 1) * sizeof(Char), false));  // released with free()
  if (!p) return nullptr;
  std::memcpy(p, s.data(), s.size() * sizeof(Char));
  p[s.size()] = 0;
  return p;
}

char* XBEM_GUEST_ABI hle__Getdays() { return heap_string(joined_names(kCDayAbbr, kCDay, 7)); }
char* XBEM_GUEST_ABI hle__Getmonths() { return heap_string(joined_names(kCMonAbbr, kCMon, 12)); }
char16_t* XBEM_GUEST_ABI hle__W_Getdays() { return heap_string(crt::to_wide(joined_names(kCDayAbbr, kCDay, 7))); }
char16_t* XBEM_GUEST_ABI hle__W_Getmonths() {
  return heap_string(crt::to_wide(joined_names(kCMonAbbr, kCMon, 12)));
}

// __crt_lc_time_data for the C locale, allocated like the UCRT does (the
// caller releases it with free()). Pointers refer to static strings.
void* make_time_names() {
  constexpr size_t kSize = 0x300;
  auto* p = static_cast<uint64_t*>(heap_alloc(kSize, true));
  if (!p) return nullptr;
  static const char* narrow_names[43];
  static std::u16string wide_storage[44];
  static std::once_flag once;
  std::call_once(once, [] {
    int k = 0;
    for (auto* n : kCDayAbbr) narrow_names[k++] = n;
    for (auto* n : kCDay) narrow_names[k++] = n;
    for (auto* n : kCMonAbbr) narrow_names[k++] = n;
    for (auto* n : kCMon) narrow_names[k++] = n;
    narrow_names[k++] = "AM";
    narrow_names[k++] = "PM";
    narrow_names[k++] = "MM/dd/yy";
    narrow_names[k++] = "dddd, MMMM dd, yyyy";
    narrow_names[k++] = "HH:mm:ss";
    for (int i = 0; i < 43; ++i) wide_storage[i] = crt::to_wide(narrow_names[i]);
    wide_storage[43] = u"en-US";
  });
  for (int i = 0; i < 43; ++i) p[i] = reinterpret_cast<uint64_t>(narrow_names[i]);
  const int32_t caltype = 1, refcount = 0;
  std::memcpy(p + 43, &caltype, 4);
  std::memcpy(reinterpret_cast<uint8_t*>(p + 43) + 4, &refcount, 4);
  for (int i = 0; i < 44; ++i) p[44 + i] = reinterpret_cast<uint64_t>(wide_storage[i].c_str());
  return p;
}

void* XBEM_GUEST_ABI hle__Gettnames() { return make_time_names(); }
void* XBEM_GUEST_ABI hle__W_Gettnames() { return make_time_names(); }

SIZE_T XBEM_GUEST_ABI hle__Strftime(char* buf, SIZE_T size, const char* fmt, const GuestTm* g, void*) {
  return hle_strftime(buf, size, fmt, g);
}
SIZE_T XBEM_GUEST_ABI hle__Wcsftime(char16_t* buf, SIZE_T size, const char16_t* fmt, const GuestTm* g, void*) {
  return hle_wcsftime(buf, size, fmt, g);
}

using Compare = int32_t(XBEM_GUEST_ABI*)(const void*, const void*);
using CompareS = int32_t(XBEM_GUEST_ABI*)(void*, const void*, const void*);

template <class Less>
void sort_elements(void* base, SIZE_T num, SIZE_T size, Less less) {
  if (!base || num < 2 || size == 0) return;
  auto* b = static_cast<uint8_t*>(base);
  std::vector<size_t> order(num);
  std::iota(order.begin(), order.end(), size_t{0});
  // Merge sort tolerates inconsistent guest comparators without going out of bounds.
  std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) { return less(b + x * size, b + y * size); });
  std::vector<uint8_t> tmp(num * size);
  for (size_t i = 0; i < num; ++i) std::memcpy(tmp.data() + i * size, b + order[i] * size, size);
  std::memcpy(b, tmp.data(), tmp.size());
}

void XBEM_GUEST_ABI hle_qsort(void* base, SIZE_T num, SIZE_T size, Compare cmp) {
  sort_elements(base, num, size, [&](const void* a, const void* b) { return cmp(a, b) < 0; });
}

void XBEM_GUEST_ABI hle_qsort_s(void* base, SIZE_T num, SIZE_T size, CompareS cmp, void* ctx) {
  sort_elements(base, num, size, [&](const void* a, const void* b) { return cmp(ctx, a, b) < 0; });
}

template <class Cmp>
void* binary_search(const void* key, const void* base, SIZE_T num, SIZE_T size, Cmp cmp) {
  const auto* b = static_cast<const uint8_t*>(base);
  SIZE_T lo = 0, hi = num;
  while (lo < hi) {
    const SIZE_T mid = lo + (hi - lo) / 2;
    const int32_t r = cmp(key, b + mid * size);
    if (r == 0) return const_cast<uint8_t*>(b + mid * size);
    if (r < 0) hi = mid;
    else lo = mid + 1;
  }
  return nullptr;
}

void* XBEM_GUEST_ABI hle_bsearch(const void* key, const void* base, SIZE_T num, SIZE_T size, Compare cmp) {
  return binary_search(key, base, num, size, [&](const void* a, const void* b) { return cmp(a, b); });
}

void* XBEM_GUEST_ABI hle_bsearch_s(const void* key, const void* base, SIZE_T num, SIZE_T size, CompareS cmp, void* ctx) {
  return binary_search(key, base, num, size, [&](const void* a, const void* b) { return cmp(ctx, a, b); });
}

// ----------------------------------------------------------- random

thread_local uint32_t t_rand_state = 1;
void XBEM_GUEST_ABI hle_srand(uint32_t seed) { t_rand_state = seed; }
int32_t XBEM_GUEST_ABI hle_rand() {  // the MSVC linear congruential generator
  t_rand_state = t_rand_state * 214013u + 2531011u;
  return static_cast<int32_t>((t_rand_state >> 16) & 0x7FFF);
}
int32_t XBEM_GUEST_ABI hle_rand_s(uint32_t* out) {
  if (!out) return crt::kEINVAL;
  static thread_local std::mt19937 gen{std::random_device{}()};
  *out = static_cast<uint32_t>(gen());
  return 0;
}

uint32_t XBEM_GUEST_ABI hle__rotl(uint32_t v, int32_t s) { return (v << (s & 31)) | (v >> ((32 - s) & 31)); }
uint32_t XBEM_GUEST_ABI hle__rotr(uint32_t v, int32_t s) { return (v >> (s & 31)) | (v << ((32 - s) & 31)); }
uint64_t XBEM_GUEST_ABI hle__rotl64(uint64_t v, int32_t s) { return (v << (s & 63)) | (v >> ((64 - s) & 63)); }
uint64_t XBEM_GUEST_ABI hle__rotr64(uint64_t v, int32_t s) { return (v >> (s & 63)) | (v << ((64 - s) & 63)); }
uint16_t XBEM_GUEST_ABI hle__byteswap_ushort(uint16_t v) { return static_cast<uint16_t>((v >> 8) | (v << 8)); }
uint32_t XBEM_GUEST_ABI hle__byteswap_ulong(uint32_t v) {
  return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}
uint64_t XBEM_GUEST_ABI hle__byteswap_uint64(uint64_t v) {
  return (static_cast<uint64_t>(hle__byteswap_ulong(static_cast<uint32_t>(v))) << 32) | hle__byteswap_ulong(static_cast<uint32_t>(v >> 32));
}

// ---------------------------------------------------------- environment

std::mutex g_env_cache_mutex;
std::map<std::string, std::string> g_env_narrow;     // stable storage for getenv results
std::map<std::string, std::u16string> g_env_wide;

char* XBEM_GUEST_ABI hle_getenv(const char* name) {
  if (!name) return nullptr;
  const auto v = crt::env_get(name);
  if (!v) return nullptr;
  std::lock_guard lock(g_env_cache_mutex);
  auto& slot = g_env_narrow[name];
  slot = crt::to_narrow(*v);
  return slot.data();
}

char16_t* XBEM_GUEST_ABI hle__wgetenv(const char16_t* name) {
  if (!name) return nullptr;
  const std::string n = narrow(name);
  const auto v = crt::env_get(n);
  if (!v) return nullptr;
  std::lock_guard lock(g_env_cache_mutex);
  auto& slot = g_env_wide[n];
  slot = *v;
  return slot.data();
}

int32_t XBEM_GUEST_ABI hle_getenv_s(SIZE_T* required, char* buf, SIZE_T size, const char* name) {
  if (!required || !name) return crt::kEINVAL;
  const auto v = crt::env_get(name);
  if (!v) {
    *required = 0;
    if (buf && size) buf[0] = 0;
    return 0;
  }
  const std::string s = crt::to_narrow(*v);
  *required = s.size() + 1;
  if (!buf || size == 0) return 0;
  if (size < s.size() + 1) return crt::kERANGE;
  std::memcpy(buf, s.c_str(), s.size() + 1);
  return 0;
}

int32_t XBEM_GUEST_ABI hle__dupenv_s(char** out, SIZE_T* len, const char* name) {
  if (!out || !name) return crt::kEINVAL;
  *out = nullptr;
  if (len) *len = 0;
  const auto v = crt::env_get(name);
  if (!v) return 0;
  const std::string s = crt::to_narrow(*v);
  auto* p = static_cast<char*>(heap_alloc(s.size() + 1, false));
  if (!p) return crt::kENOMEM;
  std::memcpy(p, s.c_str(), s.size() + 1);
  *out = p;
  if (len) *len = s.size() + 1;
  return 0;
}

int32_t XBEM_GUEST_ABI hle__wdupenv_s(char16_t** out, SIZE_T* len, const char16_t* name) {
  if (!out || !name) return crt::kEINVAL;
  *out = nullptr;
  if (len) *len = 0;
  const auto v = crt::env_get(narrow(name));
  if (!v) return 0;
  auto* p = static_cast<char16_t*>(heap_alloc((v->size() + 1) * 2, false));
  if (!p) return crt::kENOMEM;
  std::memcpy(p, v->c_str(), (v->size() + 1) * 2);
  *out = p;
  if (len) *len = v->size() + 1;
  return 0;
}

int32_t put_env(const std::string& assignment) {
  const size_t eq = assignment.find('=');
  if (eq == std::string::npos || eq == 0) return -1;
  const std::string value = assignment.substr(eq + 1);
  crt::env_set(assignment.substr(0, eq),
               value.empty() ? std::nullopt : std::optional<std::u16string>(crt::to_wide(value)));
  return 0;
}

int32_t XBEM_GUEST_ABI hle__putenv(const char* s) { return s ? put_env(s) : -1; }
int32_t XBEM_GUEST_ABI hle__wputenv(const char16_t* s) { return s ? put_env(narrow(s)) : -1; }
int32_t XBEM_GUEST_ABI hle__putenv_s(const char* n, const char* v) {
  return (n && v) ? put_env(std::string(n) + "=" + v) == 0 ? 0 : crt::kEINVAL : crt::kEINVAL;
}
int32_t XBEM_GUEST_ABI hle__wputenv_s(const char16_t* n, const char16_t* v) {
  return (n && v) ? put_env(narrow(n) + "=" + narrow(v)) == 0 ? 0 : crt::kEINVAL : crt::kEINVAL;
}

// ---------------------------------------------------------- file system

std::optional<rt::Vfs::Resolved> resolve(const std::string& path) {
  auto r = proc().vfs().resolve(std::string_view(path));
  if (!r) set_errno(crt::kENOENT);
  return r;
}

int64_t to_unix_time(fs::file_time_type t) {
  using namespace std::chrono;
  return duration_cast<seconds>(clock_cast<system_clock>(t).time_since_epoch()).count();
}

struct StatInfo {
  uint16_t mode;
  int64_t size;
  int64_t mtime;
};

bool stat_path(const std::string& path, StatInfo& out) {
  auto r = resolve(path);
  std::error_code ec;
  if (!r || !fs::exists(r->host_path, ec)) {
    set_errno(crt::kENOENT);
    return false;
  }
  const bool dir = fs::is_directory(r->host_path, ec);
  out.mode = dir ? 0x4000 /*_S_IFDIR*/ | 0x0040 /*_S_IEXEC*/ : 0x8000 /*_S_IFREG*/;
  out.mode |= 0x0100;                     // _S_IREAD
  if (!r->read_only) out.mode |= 0x0080;  // _S_IWRITE
  out.size = dir ? 0 : static_cast<int64_t>(fs::file_size(r->host_path, ec));
  out.mtime = to_unix_time(fs::last_write_time(r->host_path, ec));
  return true;
}

// struct _stat64 (56 bytes) and _stat64i32 (48 bytes): dev ino mode nlink uid gid rdev size atime mtime ctime
void fill_stat(uint8_t* out, const StatInfo& s, bool size32) {
  std::memset(out, 0, size32 ? 48 : 56);
  const int16_t nlink = 1;
  const uint32_t dev = 6;  // drive G:
  std::memcpy(out + 0, &dev, 4);
  std::memcpy(out + 6, &s.mode, 2);
  std::memcpy(out + 8, &nlink, 2);
  std::memcpy(out + 16, &dev, 4);
  if (size32) {
    const auto sz = static_cast<int32_t>(s.size);
    std::memcpy(out + 20, &sz, 4);
    for (int i = 0; i < 3; ++i) std::memcpy(out + 24 + i * 8, &s.mtime, 8);
  } else {
    std::memcpy(out + 24, &s.size, 8);
    for (int i = 0; i < 3; ++i) std::memcpy(out + 32 + i * 8, &s.mtime, 8);
  }
}

int32_t XBEM_GUEST_ABI hle__stat64(const char* path, uint8_t* buf) {
  StatInfo s{};
  if (!path || !buf || !stat_path(narrow(path), s)) return -1;
  fill_stat(buf, s, false);
  return 0;
}
int32_t XBEM_GUEST_ABI hle__wstat64(const char16_t* path, uint8_t* buf) {
  StatInfo s{};
  if (!path || !buf || !stat_path(narrow(path), s)) return -1;
  fill_stat(buf, s, false);
  return 0;
}
int32_t XBEM_GUEST_ABI hle__stat64i32(const char* path, uint8_t* buf) {
  StatInfo s{};
  if (!path || !buf || !stat_path(narrow(path), s)) return -1;
  fill_stat(buf, s, true);
  return 0;
}
int32_t XBEM_GUEST_ABI hle__wstat64i32(const char16_t* path, uint8_t* buf) {
  StatInfo s{};
  if (!path || !buf || !stat_path(narrow(path), s)) return -1;
  fill_stat(buf, s, true);
  return 0;
}

int32_t fstat_fd(int32_t fd, uint8_t* buf, bool size32) {
  crt::GuestFile* f = crt::file_from_fd(fd);
  if (!f || !buf) {
    set_errno(crt::kEBADF);
    return -1;
  }
  StatInfo s{};
  s.mode = fd <= 2 ? 0x2000 /*_S_IFCHR*/ : 0x8000 | 0x0100;
  if (fd > 2) {
    std::fflush(f->host);
#if defined(_WIN32)
    const int64_t cur = _ftelli64(f->host);
    _fseeki64(f->host, 0, SEEK_END);
    s.size = _ftelli64(f->host);
    _fseeki64(f->host, cur, SEEK_SET);
#else
    const off_t cur = ftello(f->host);
    fseeko(f->host, 0, SEEK_END);
    s.size = ftello(f->host);
    fseeko(f->host, cur, SEEK_SET);
#endif
  }
  fill_stat(buf, s, size32);
  return 0;
}
int32_t XBEM_GUEST_ABI hle__fstat64(int32_t fd, uint8_t* buf) { return fstat_fd(fd, buf, false); }
int32_t XBEM_GUEST_ABI hle__fstat64i32(int32_t fd, uint8_t* buf) { return fstat_fd(fd, buf, true); }

int32_t access_path(const std::string& path, int32_t mode) {
  auto r = resolve(path);
  std::error_code ec;
  if (!r || !fs::exists(r->host_path, ec)) {
    set_errno(crt::kENOENT);
    return -1;
  }
  if ((mode & 2) && r->read_only) {
    set_errno(crt::kEACCES);
    return -1;
  }
  return 0;
}

int32_t XBEM_GUEST_ABI hle__access(const char* p, int32_t m) { return p ? access_path(narrow(p), m) : -1; }
int32_t XBEM_GUEST_ABI hle__waccess(const char16_t* p, int32_t m) { return p ? access_path(narrow(p), m) : -1; }
int32_t XBEM_GUEST_ABI hle__access_s(const char* p, int32_t m) {
  return hle__access(p, m) == 0 ? 0 : *crt::errno_ptr();
}
int32_t XBEM_GUEST_ABI hle__waccess_s(const char16_t* p, int32_t m) {
  return hle__waccess(p, m) == 0 ? 0 : *crt::errno_ptr();
}

std::optional<rt::Vfs::Resolved> resolve_writable(const std::string& path) {
  auto r = resolve(path);
  if (r && r->read_only) {
    set_errno(crt::kEACCES);
    return std::nullopt;
  }
  return r;
}

int32_t make_dir(const std::string& path) {
  auto r = resolve_writable(path);
  std::error_code ec;
  if (!r) return -1;
  if (fs::exists(r->host_path, ec)) {
    set_errno(crt::kEEXIST);
    return -1;
  }
  if (!fs::create_directory(r->host_path, ec)) {
    set_errno(crt::kENOENT);
    return -1;
  }
  return 0;
}

int32_t remove_path(const std::string& path, bool dir) {
  auto r = resolve_writable(path);
  std::error_code ec;
  if (!r) return -1;
  if (!fs::exists(r->host_path, ec) || fs::is_directory(r->host_path, ec) != dir) {
    set_errno(crt::kENOENT);
    return -1;
  }
  if (!fs::remove(r->host_path, ec)) {
    set_errno(crt::kEACCES);
    return -1;
  }
  return 0;
}

int32_t rename_path(const std::string& from, const std::string& to) {
  auto a = resolve_writable(from);
  auto b = resolve_writable(to);
  if (!a || !b) return -1;
  std::error_code ec;
  if (fs::exists(b->host_path, ec)) {
    set_errno(crt::kEEXIST);  // Windows rename() does not replace existing files
    return -1;
  }
  fs::rename(a->host_path, b->host_path, ec);
  if (ec) {
    set_errno(crt::kENOENT);
    return -1;
  }
  return 0;
}

int32_t XBEM_GUEST_ABI hle__mkdir(const char* p) { return p ? make_dir(narrow(p)) : -1; }
int32_t XBEM_GUEST_ABI hle__wmkdir(const char16_t* p) { return p ? make_dir(narrow(p)) : -1; }
int32_t XBEM_GUEST_ABI hle__rmdir(const char* p) { return p ? remove_path(narrow(p), true) : -1; }
int32_t XBEM_GUEST_ABI hle__wrmdir(const char16_t* p) { return p ? remove_path(narrow(p), true) : -1; }
int32_t XBEM_GUEST_ABI hle_remove(const char* p) { return p ? remove_path(narrow(p), false) : -1; }
int32_t XBEM_GUEST_ABI hle__wremove(const char16_t* p) { return p ? remove_path(narrow(p), false) : -1; }
int32_t XBEM_GUEST_ABI hle_rename(const char* a, const char* b) { return (a && b) ? rename_path(narrow(a), narrow(b)) : -1; }
int32_t XBEM_GUEST_ABI hle__wrename(const char16_t* a, const char16_t* b) {
  return (a && b) ? rename_path(narrow(a), narrow(b)) : -1;
}

// _findfirst/_findnext: handles are indices into a table of directory listings.
struct FindState {
  std::vector<fs::directory_entry> entries;
  size_t next = 0;
  bool read_only = false;
};
std::mutex g_find_mutex;
std::map<int64_t, FindState> g_finds;
int64_t g_next_find = 1;

enum class FindLayout { Narrow64, Narrow64i32, Wide64, Wide64i32 };

void fill_find(uint8_t* out, const fs::directory_entry& e, bool read_only, FindLayout layout) {
  const bool wide = layout == FindLayout::Wide64 || layout == FindLayout::Wide64i32;
  const bool size32 = layout == FindLayout::Narrow64i32 || layout == FindLayout::Wide64i32;
  const size_t name_offset = size32 ? 36 : 40;
  std::memset(out, 0, name_offset + (wide ? 520 : 260));
  std::error_code ec;
  const bool dir = e.is_directory(ec);
  uint32_t attrib = dir ? 0x10 : 0x20;  // _A_SUBDIR / _A_ARCH
  if (read_only) attrib |= 0x01;        // _A_RDONLY
  std::memcpy(out, &attrib, 4);
  const int64_t t = to_unix_time(e.last_write_time(ec));
  for (int i = 0; i < 3; ++i) std::memcpy(out + 8 + i * 8, &t, 8);
  const int64_t size = dir ? 0 : static_cast<int64_t>(e.file_size(ec));
  if (size32) {
    const auto s32 = static_cast<uint32_t>(size);
    std::memcpy(out + 32, &s32, 4);
  } else {
    std::memcpy(out + 32, &size, 8);
  }
  const std::string name = e.path().filename().string();
  if (wide) {
    const std::u16string w = crt::to_wide(name);
    std::memcpy(out + name_offset, w.data(), std::min<size_t>(w.size(), 259) * 2);
  } else {
    std::memcpy(out + name_offset, name.data(), std::min<size_t>(name.size(), 259));
  }
}

int64_t find_first(const std::string& pattern_path, uint8_t* out, FindLayout layout) {
  const size_t cut = pattern_path.find_last_of("\\/");
  const std::string dir = cut == std::string::npos ? "." : pattern_path.substr(0, cut + 1);
  const std::string pattern = cut == std::string::npos ? pattern_path : pattern_path.substr(cut + 1);
  auto r = resolve(dir);
  std::error_code ec;
  if (!r || !fs::is_directory(r->host_path, ec)) {
    set_errno(crt::kENOENT);
    return -1;
  }
  FindState st;
  st.read_only = r->read_only;
  for (fs::directory_iterator it(r->host_path, ec), end; !ec && it != end; it.increment(ec))
    if (wildcard_match_icase(pattern, it->path().filename().string())) st.entries.push_back(*it);
  if (st.entries.empty()) {
    set_errno(crt::kENOENT);
    return -1;
  }
  fill_find(out, st.entries[0], st.read_only, layout);
  st.next = 1;
  std::lock_guard lock(g_find_mutex);
  const int64_t h = g_next_find++;
  g_finds[h] = std::move(st);
  return h;
}

int32_t find_next(int64_t h, uint8_t* out, FindLayout layout) {
  std::lock_guard lock(g_find_mutex);
  auto it = g_finds.find(h);
  if (it == g_finds.end()) {
    set_errno(crt::kEINVAL);
    return -1;
  }
  if (it->second.next >= it->second.entries.size()) {
    set_errno(crt::kENOENT);
    return -1;
  }
  fill_find(out, it->second.entries[it->second.next++], it->second.read_only, layout);
  return 0;
}

int64_t XBEM_GUEST_ABI hle__findfirst64(const char* p, uint8_t* out) { return p ? find_first(narrow(p), out, FindLayout::Narrow64) : -1; }
int64_t XBEM_GUEST_ABI hle__findfirst64i32(const char* p, uint8_t* out) { return p ? find_first(narrow(p), out, FindLayout::Narrow64i32) : -1; }
int64_t XBEM_GUEST_ABI hle__wfindfirst64(const char16_t* p, uint8_t* out) { return p ? find_first(narrow(p), out, FindLayout::Wide64) : -1; }
int64_t XBEM_GUEST_ABI hle__wfindfirst64i32(const char16_t* p, uint8_t* out) { return p ? find_first(narrow(p), out, FindLayout::Wide64i32) : -1; }
int32_t XBEM_GUEST_ABI hle__findnext64(int64_t h, uint8_t* out) { return find_next(h, out, FindLayout::Narrow64); }
int32_t XBEM_GUEST_ABI hle__findnext64i32(int64_t h, uint8_t* out) { return find_next(h, out, FindLayout::Narrow64i32); }
int32_t XBEM_GUEST_ABI hle__wfindnext64(int64_t h, uint8_t* out) { return find_next(h, out, FindLayout::Wide64); }
int32_t XBEM_GUEST_ABI hle__wfindnext64i32(int64_t h, uint8_t* out) { return find_next(h, out, FindLayout::Wide64i32); }
int32_t XBEM_GUEST_ABI hle__findclose(int64_t h) {
  std::lock_guard lock(g_find_mutex);
  return g_finds.erase(h) ? 0 : -1;
}

template <class C>
C* get_cwd(C* buf, int32_t size) {
  const C root[] = {static_cast<C>(proc().vfs().default_drive()), ':', '\\', 0};
  if (!buf) {
    buf = static_cast<C*>(heap_alloc(sizeof root, false));
    size = 4;
  }
  if (!buf || size < 4) {
    set_errno(crt::kERANGE);
    return nullptr;
  }
  std::memcpy(buf, root, sizeof root);
  return buf;
}
char* XBEM_GUEST_ABI hle__getcwd(char* b, int32_t n) { return get_cwd(b, n); }
char16_t* XBEM_GUEST_ABI hle__wgetcwd(char16_t* b, int32_t n) { return get_cwd(b, n); }
int32_t XBEM_GUEST_ABI hle__chdir(const char*) { return 0; }
int32_t XBEM_GUEST_ABI hle__wchdir(const char16_t*) { return 0; }
int32_t XBEM_GUEST_ABI hle__getdrive() { return proc().vfs().default_drive() - 'A' + 1; }

template <class C>
C* full_path(C* out, const C* rel, SIZE_T size) {
  if (!rel) return nullptr;
  std::basic_string<C> p(rel);
  for (C& c : p)
    if (c == '/') c = '\\';
  if (!(p.size() >= 2 && p[1] == ':')) {
    std::basic_string<C> root{static_cast<C>(proc().vfs().default_drive()), ':'};
    p = (!p.empty() && p[0] == '\\') ? root + p : root + static_cast<C>('\\') + p;
  }
  if (!out) {
    out = static_cast<C*>(heap_alloc((p.size() + 1) * sizeof(C), false));
    size = p.size() + 1;
  }
  if (!out || p.size() + 1 > size) {
    set_errno(crt::kERANGE);
    return nullptr;
  }
  std::memcpy(out, p.c_str(), (p.size() + 1) * sizeof(C));
  return out;
}
char* XBEM_GUEST_ABI hle__fullpath(char* out, const char* rel, SIZE_T n) { return full_path(out, rel, n); }
char16_t* XBEM_GUEST_ABI hle__wfullpath(char16_t* out, const char16_t* rel, SIZE_T n) { return full_path(out, rel, n); }

template <class C>
int32_t put_part(C* dst, SIZE_T size, const C* begin, const C* end) {
  if (!dst) return 0;
  const size_t n = static_cast<size_t>(end - begin);
  if (n + 1 > size) {
    if (size) dst[0] = 0;
    return crt::kERANGE;
  }
  std::memcpy(dst, begin, n * sizeof(C));
  dst[n] = 0;
  return 0;
}

template <class C>
int32_t split_path(const C* path, C* drive, SIZE_T dsz, C* dir, SIZE_T dirsz, C* fname, SIZE_T fsz, C* ext,
                   SIZE_T esz) {
  if (!path) return crt::kEINVAL;
  const C* p = path;
  const C* end = p;
  while (*end) ++end;
  const C* after_drive = (end - p >= 2 && p[1] == ':') ? p + 2 : p;
  const C* last_sep = nullptr;
  for (const C* q = after_drive; q < end; ++q)
    if (*q == '\\' || *q == '/') last_sep = q;
  const C* name_begin = last_sep ? last_sep + 1 : after_drive;
  const C* dot = nullptr;
  for (const C* q = name_begin; q < end; ++q)
    if (*q == '.') dot = q;
  const C* name_end = dot ? dot : end;
  int32_t r = 0;
  r |= put_part(drive, dsz, p, after_drive);
  r |= put_part(dir, dirsz, after_drive, name_begin);
  r |= put_part(fname, fsz, name_begin, name_end);
  r |= put_part(ext, esz, name_end, end);
  return r ? crt::kERANGE : 0;
}

int32_t XBEM_GUEST_ABI hle__splitpath_s(const char* p, char* d, SIZE_T ds, char* dir, SIZE_T dirs, char* f, SIZE_T fs_,
                                        char* e, SIZE_T es) {
  return split_path(p, d, ds, dir, dirs, f, fs_, e, es);
}
int32_t XBEM_GUEST_ABI hle__wsplitpath_s(const char16_t* p, char16_t* d, SIZE_T ds, char16_t* dir, SIZE_T dirs,
                                         char16_t* f, SIZE_T fs_, char16_t* e, SIZE_T es) {
  return split_path(p, d, ds, dir, dirs, f, fs_, e, es);
}
void XBEM_GUEST_ABI hle__splitpath(const char* p, char* d, char* dir, char* f, char* e) {
  split_path(p, d, 3, dir, 256, f, 256, e, 256);
}
void XBEM_GUEST_ABI hle__wsplitpath(const char16_t* p, char16_t* d, char16_t* dir, char16_t* f, char16_t* e) {
  split_path(p, d, 3, dir, 256, f, 256, e, 256);
}

template <class C>
int32_t make_path(C* out, SIZE_T size, const C* drive, const C* dir, const C* fname, const C* ext) {
  if (!out) return crt::kEINVAL;
  std::basic_string<C> s;
  if (drive && *drive) {
    s.push_back(drive[0]);
    s.push_back(':');
  }
  if (dir && *dir) {
    s += dir;
    if (s.back() != '\\' && s.back() != '/') s.push_back('\\');
  }
  if (fname) s += fname;
  if (ext && *ext) {
    if (ext[0] != '.') s.push_back('.');
    s += ext;
  }
  if (s.size() + 1 > size) {
    if (size) out[0] = 0;
    return crt::kERANGE;
  }
  std::memcpy(out, s.c_str(), (s.size() + 1) * sizeof(C));
  return 0;
}
int32_t XBEM_GUEST_ABI hle__makepath_s(char* o, SIZE_T n, const char* d, const char* dir, const char* f, const char* e) {
  return make_path(o, n, d, dir, f, e);
}
int32_t XBEM_GUEST_ABI hle__wmakepath_s(char16_t* o, SIZE_T n, const char16_t* d, const char16_t* dir,
                                        const char16_t* f, const char16_t* e) {
  return make_path(o, n, d, dir, f, e);
}

}  // namespace

void add_ucrt_misc_exports(std::vector<ExportDef>& out) {
  out.insert(out.end(), {
      XBEM_HLE(_time64), XBEM_HLE(_time32), ExportDef{"time", reinterpret_cast<void*>(&hle__time64)},
      XBEM_HLE(_gmtime64_s), XBEM_HLE(_localtime64_s), XBEM_HLE(_gmtime64), XBEM_HLE(_localtime64),
      ExportDef{"gmtime", reinterpret_cast<void*>(&hle__gmtime64)},
      ExportDef{"localtime", reinterpret_cast<void*>(&hle__localtime64)}, XBEM_HLE(_mktime64),
      ExportDef{"mktime", reinterpret_cast<void*>(&hle__mktime64)}, XBEM_HLE(_mkgmtime64), XBEM_HLE(_difftime64),
      XBEM_HLE(clock), XBEM_HLE(strftime), XBEM_HLE(wcsftime), XBEM_HLE(_tzset), XBEM_HLE(_get_timezone),
      XBEM_HLE(_get_daylight), XBEM_HLE(_get_dstbias), XBEM_HLE(_timespec64_get),
      ExportDef{"timespec_get", reinterpret_cast<void*>(&hle__timespec64_get)}, XBEM_HLE(_ftime64_s),
      XBEM_HLE(_ftime64), XBEM_HLE(_Getdays), XBEM_HLE(_Getmonths), XBEM_HLE(_W_Getdays), XBEM_HLE(_W_Getmonths),
      XBEM_HLE(_Gettnames), XBEM_HLE(_W_Gettnames), XBEM_HLE(_Strftime), XBEM_HLE(_Wcsftime),
      XBEM_HLE(qsort), XBEM_HLE(qsort_s), XBEM_HLE(bsearch), XBEM_HLE(bsearch_s), XBEM_HLE(srand), XBEM_HLE(rand),
      XBEM_HLE(rand_s), XBEM_HLE(_rotl), XBEM_HLE(_rotr), XBEM_HLE(_rotl64), XBEM_HLE(_rotr64),
      ExportDef{"_lrotl", reinterpret_cast<void*>(&hle__rotl)}, ExportDef{"_lrotr", reinterpret_cast<void*>(&hle__rotr)},
      XBEM_HLE(_byteswap_ushort), XBEM_HLE(_byteswap_ulong), XBEM_HLE(_byteswap_uint64), XBEM_HLE(getenv),
      XBEM_HLE(_wgetenv), XBEM_HLE(getenv_s), XBEM_HLE(_dupenv_s), XBEM_HLE(_wdupenv_s), XBEM_HLE(_putenv),
      XBEM_HLE(_wputenv), XBEM_HLE(_putenv_s), XBEM_HLE(_wputenv_s), XBEM_HLE(_stat64), XBEM_HLE(_wstat64),
      XBEM_HLE(_stat64i32), XBEM_HLE(_wstat64i32), XBEM_HLE(_fstat64), XBEM_HLE(_fstat64i32), XBEM_HLE(_access),
      XBEM_HLE(_waccess), XBEM_HLE(_access_s), XBEM_HLE(_waccess_s), XBEM_HLE(_mkdir), XBEM_HLE(_wmkdir),
      XBEM_HLE(_rmdir), XBEM_HLE(_wrmdir), XBEM_HLE(remove), XBEM_HLE(_wremove), XBEM_HLE(rename),
      XBEM_HLE(_wrename), ExportDef{"_unlink", reinterpret_cast<void*>(&hle_remove)},
      ExportDef{"_wunlink", reinterpret_cast<void*>(&hle__wremove)}, XBEM_HLE(_findfirst64),
      XBEM_HLE(_findfirst64i32), XBEM_HLE(_wfindfirst64), XBEM_HLE(_wfindfirst64i32), XBEM_HLE(_findnext64),
      XBEM_HLE(_findnext64i32), XBEM_HLE(_wfindnext64), XBEM_HLE(_wfindnext64i32), XBEM_HLE(_findclose),
      XBEM_HLE(_getcwd), XBEM_HLE(_wgetcwd), XBEM_HLE(_chdir), XBEM_HLE(_wchdir), XBEM_HLE(_getdrive),
      XBEM_HLE(_fullpath), XBEM_HLE(_wfullpath), XBEM_HLE(_splitpath_s), XBEM_HLE(_wsplitpath_s),
      XBEM_HLE(_splitpath), XBEM_HLE(_wsplitpath), XBEM_HLE(_makepath_s), XBEM_HLE(_wmakepath_s),
  });
}

}  // namespace xbem::hle
