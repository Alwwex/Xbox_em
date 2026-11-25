/* The C runtime (UCRT) as an MSVC-built title sees it: imported through the
 * api-ms-win-crt-* API sets and with Windows semantics (32-bit long, 16-bit
 * wchar_t, MSVC printf extensions, MSVC rand(), text-mode CRLF handling). */
#include <stdarg.h>

#include "guest.h"

typedef uint16_t wchar;
#define W(s) ((const wchar*)L##s)

/* api-ms-win-crt-stdio */
IMP int __stdio_common_vsprintf(uint64_t options, char* buf, size_t n, const char* fmt, void* locale, va_list ap);
IMP int __stdio_common_vswprintf(uint64_t options, wchar* buf, size_t n, const wchar* fmt, void* locale, va_list ap);
IMP int __stdio_common_vsscanf(uint64_t options, const char* in, size_t n, const char* fmt, void* locale, va_list ap);
IMP int __stdio_common_vswscanf(uint64_t options, const wchar* in, size_t n, const wchar* fmt, void* locale, va_list ap);
IMP void* fopen(const char*, const char*);
IMP int fclose(void*);
IMP size_t fwrite(const void*, size_t, size_t, void*);
IMP size_t fread(void*, size_t, size_t, void*);
IMP char* fgets(char*, int, void*);
/* api-ms-win-crt-string */
IMP size_t strlen(const char*);
IMP int strcmp(const char*, const char*);
IMP int _stricmp(const char*, const char*);
IMP int strcpy_s(char*, size_t, const char*);
IMP char* strtok_s(char*, const char*, char**);
IMP size_t wcslen(const wchar*);
IMP int _wcsicmp(const wchar*, const wchar*);
IMP int isalpha(int);
IMP int toupper(int);
IMP unsigned towupper(unsigned);
/* api-ms-win-crt-convert */
IMP int32_t strtol(const char*, char**, int);
IMP uint32_t strtoul(const char*, char**, int);
IMP double strtod(const char*, char**);
IMP int atoi(const char*);
IMP int _itoa_s(int, char*, size_t, int);
IMP int _ui64toa_s(uint64_t, char*, size_t, int);
IMP int32_t wcstol(const wchar*, wchar**, int);
/* api-ms-win-crt-heap */
IMP void* malloc(size_t);
IMP void* calloc(size_t, size_t);
IMP void* realloc(void*, size_t);
IMP void free(void*);
IMP size_t _msize(void*);
IMP void* _aligned_malloc(size_t, size_t);
IMP void _aligned_free(void*);
/* api-ms-win-crt-math */
IMP double sqrt(double);
IMP double pow(double, double);
IMP double floor(double);
IMP short _dclass(double);
IMP int _controlfp_s(unsigned*, unsigned, unsigned);
/* api-ms-win-crt-utility */
IMP void qsort(void*, size_t, size_t, int (*)(const void*, const void*));
IMP void* bsearch(const void*, const void*, size_t, size_t, int (*)(const void*, const void*));
IMP int rand(void);
IMP void srand(unsigned);
/* api-ms-win-crt-runtime */
IMP int* _errno(void);
/* api-ms-win-crt-time */
IMP int64_t _time64(int64_t*);
IMP int _gmtime64_s(int* tm, const int64_t*);
IMP size_t strftime(char*, size_t, const char*, const int* tm);
IMP char* _Getdays(void);
IMP char* _Getmonths(void);
IMP void* _Gettnames(void);
IMP size_t _Strftime(char*, size_t, const char*, const int* tm, void*);
IMP void* _aligned_offset_malloc(size_t, size_t, size_t);
IMP size_t __strncnt(const char*, size_t);
IMP float nexttowardf(float, double);
/* api-ms-win-crt-filesystem / environment */
IMP int _stat64(const char*, void*);
IMP int _access(const char*, int);
IMP char* getenv(const char*);
IMP int _putenv_s(const char*, const char*);
/* kernelx */
IMP DWORD GetEnvironmentVariableA(const char*, char*, DWORD);

#define STANDARD_SNPRINTF 2u

static int fmt(char* buf, size_t n, uint64_t options, const char* f, ...) {
  va_list ap;
  va_start(ap, f);
  int r = __stdio_common_vsprintf(options, buf, n, f, 0, ap);
  va_end(ap);
  return r;
}

static int wfmt(wchar* buf, size_t n, const wchar* f, ...) {
  va_list ap;
  va_start(ap, f);
  int r = __stdio_common_vswprintf(STANDARD_SNPRINTF | 4u /*legacy wide specifiers*/, buf, n, f, 0, ap);
  va_end(ap);
  return r;
}

static int scan(const char* in, const char* f, ...) {
  va_list ap;
  va_start(ap, f);
  int r = __stdio_common_vsscanf(0, in, (size_t)-1, f, 0, ap);
  va_end(ap);
  return r;
}

static int wscan(const wchar* in, const wchar* f, ...) {
  va_list ap;
  va_start(ap, f);
  int r = __stdio_common_vswscanf(0, in, (size_t)-1, f, 0, ap);
  va_end(ap);
  return r;
}

static int cmp_desc(const void* a, const void* b) { return *(const int*)b - *(const int*)a; }
static int cmp_asc(const void* a, const void* b) { return *(const int*)a - *(const int*)b; }

DWORD guest_main(void* peb) {
  (void)peb;
  char buf[128];

  /* ---- printf family ---- */
  CHECK(fmt(buf, sizeof buf, STANDARD_SNPRINTF, "%d|%5s|%-4d|%05.1f|%x|%%", -5, "ab", 7, 3.14159, 255) == 24);
  say(buf);
  CHECK(gstreq(buf, "-5|   ab|7   |003.1|ff|%"));
  fmt(buf, sizeof buf, STANDARD_SNPRINTF, "%I64d %lld %llu %zu", (int64_t)-9000000000LL, 123LL, 18446744073709551615ULL,
      (size_t)42);
  CHECK(gstreq(buf, "-9000000000 123 18446744073709551615 42"));
  fmt(buf, sizeof buf, STANDARD_SNPRINTF, "%p|%ls|%S|%c|%s", (void*)0x1234, L"wide", L"too", 'Z', (char*)0);
  say(buf);
  CHECK(gstreq(buf, "0000000000001234|wide|too|Z|(null)"));
  fmt(buf, sizeof buf, STANDARD_SNPRINTF, "%ld %hd %hhu", (long)0x1FFFFFFFF, (int)0x12345, 0x1FF);
  CHECK(gstreq(buf, "-1 9029 255")); /* long is 32-bit on Windows */
  fmt(buf, sizeof buf, STANDARD_SNPRINTF, "%f %e %g", __builtin_nan(""), 1.5e10, 0.0001);
  CHECK(gstreq(buf, "nan 1.500000e+10 0.0001"));
  CHECK(fmt(buf, 5, STANDARD_SNPRINTF, "hello world") == 11 && gstreq(buf, "hell"));
  CHECK(fmt(buf, 5, 0, "hello world") == -1); /* legacy _vsnprintf truncation */
  CHECK(fmt(0, 0, STANDARD_SNPRINTF, "%d", 12345) == 5);

  wchar wb[64];
  CHECK(wfmt(wb, 64, W("%s=%d %hs"), W("key"), 7, "narrow") == 12);
  CHECK(_wcsicmp(wb, W("KEY=7 NARROW")) == 0);

  /* ---- scanf ---- */
  int i = 0;
  double d = 0;
  char word[16];
  CHECK(scan("42 3.5 hello", "%d %lf %15s", &i, &d, word) == 3);
  CHECK(i == 42 && d == 3.5 && gstreq(word, "hello"));
  unsigned hex = 0;
  CHECK(scan("ff", "%x", &hex) == 1 && hex == 255);
  CHECK(scan("", "%d", &i) == -1);
  /* %n: characters consumed so far, not counted (GTA VI parses "~0~" icon
   * tokens in text this way and advances by the count) */
  int consumed = -1;
  CHECK(scan("  12ab", "%d%n", &i, &consumed) == 1 && i == 12 && consumed == 4);
  wchar open_c = 0, close_c = 0;
  consumed = -1;
  CHECK(wscan(W("~7~ to run"), W("%lc%d%lc%n"), &open_c, &i, &close_c, &consumed) == 3);
  CHECK(open_c == '~' && i == 7 && close_c == '~' && consumed == 3);

  /* ---- conversions with Windows integer sizes ---- */
  *_errno() = 0;
  CHECK(strtol("4294967296", 0, 10) == 2147483647);
  CHECK(*_errno() == 34); /* ERANGE */
  CHECK(strtoul("-1", 0, 10) == 4294967295u);
  /* A long list read number by number (GTA VI's XML parser does this with
   * megabytes of text): each call looks only at its number. */
  {
    static char list[1 << 20];
    size_t at = 0;
    uint32_t written = 0;
    while (at + 4 < sizeof list) { list[at++] = '4'; list[at++] = '2'; list[at++] = ' '; ++written; }
    list[at] = '<';
    const char* p = list;
    char* next = 0;
    uint32_t count = 0, sum = 0;
    while (*p != '<') {
      sum += strtoul(p, &next, 10);
      CHECK(next == p + 2);
      p = next + 1;
      ++count;
    }
    CHECK(count == written && sum == count * 42);
    CHECK(strtoul(" 0x1fz", &next, 16) == 31 && *next == 'z');
  }
  char* end = 0;
  CHECK(strtod("2.5x", &end) == 2.5 && *end == 'x');
  CHECK(atoi("  -17") == -17);
  CHECK(wcstol(W("0x10"), 0, 16) == 16);
  CHECK(_itoa_s(-123, buf, sizeof buf, 10) == 0 && gstreq(buf, "-123"));
  CHECK(_ui64toa_s(255, buf, sizeof buf, 16) == 0 && gstreq(buf, "ff"));

  /* ---- strings ---- */
  CHECK(strlen("abc") == 3 && strcmp("a", "b") < 0 && _stricmp("HeLLo", "hello") == 0);
  CHECK(strcpy_s(buf, 4, "too long") == 34 && buf[0] == 0); /* ERANGE */
  char text[] = "a,b,,c";
  char* ctx = 0;
  char* t1 = strtok_s(text, ",", &ctx);
  char* t2 = strtok_s(0, ",", &ctx);
  char* t3 = strtok_s(0, ",", &ctx);
  CHECK(t1 && t2 && t3 && gstreq(t1, "a") && gstreq(t2, "b") && gstreq(t3, "c"));
  CHECK(wcslen(W("abc")) == 3);
  const wchar upper[] = {0xC4, 'B', 0};  /* "ÄB" */
  const wchar lower[] = {0xE4, 'b', 0};  /* "äb" */
  CHECK(_wcsicmp(upper, lower) == 0);
  CHECK(isalpha('x') && !isalpha('1') && toupper('q') == 'Q' && towupper(0xE9) == 0xC9);

  /* ---- heap ---- */
  char* p = (char*)malloc(100);
  CHECK(p && _msize(p) == 100);
  p = (char*)realloc(p, 1000);
  CHECK(p && _msize(p) == 1000);
  free(p);
  int* z = (int*)calloc(10, sizeof(int));
  CHECK(z && z[9] == 0);
  free(z);
  void* a = _aligned_malloc(100, 64);
  CHECK(a && ((uintptr_t)a & 63) == 0);
  _aligned_free(a);
  char* ao = (char*)_aligned_offset_malloc(100, 64, 8);
  CHECK(ao && (((uintptr_t)ao + 8) & 63) == 0);
  _aligned_free(ao);
  CHECK(__strncnt("abcdef", 4) == 4 && __strncnt("ab", 10) == 2);
  CHECK(nexttowardf(1.0f, 2.0) > 1.0f && nexttowardf(1.0f, 0.0) < 1.0f);

  /* ---- math & floating-point control ---- */
  CHECK(sqrt(16.0) == 4.0 && pow(2.0, 10.0) == 1024.0 && floor(-1.5) == -2.0);
  CHECK(_dclass(__builtin_inf()) == 1 && _dclass(0.0) == 0 && _dclass(1.0) == -1);
  unsigned cw = 0;
  CHECK(_controlfp_s(&cw, 0, 0) == 0 && (cw & 0x8001F) == 0x8001F); /* all exceptions masked */

  /* ---- sorting, searching, rand ---- */
  int v[] = {3, 9, 1, 7, 5};
  qsort(v, 5, sizeof(int), cmp_desc);
  CHECK(v[0] == 9 && v[4] == 1);
  qsort(v, 5, sizeof(int), cmp_asc);
  int key = 7;
  int* hit = (int*)bsearch(&key, v, 5, sizeof(int), cmp_asc);
  CHECK(hit && *hit == 7);
  srand(1);
  CHECK(rand() == 41); /* MSVC's LCG */

  /* ---- time ---- */
  CHECK(_time64(0) > 1700000000);
  int tm[9];
  int64_t epoch = 0;
  CHECK(_gmtime64_s(tm, &epoch) == 0 && tm[5] == 70);
  CHECK(strftime(buf, sizeof buf, "%Y-%m-%d", tm) == 10 && gstreq(buf, "1970-01-01"));
  void* names = _Gettnames();
  CHECK(names && _Strftime(buf, sizeof buf, "%Y", tm, names) == 4 && gstreq(buf, "1970"));
  free(names);
  char* days = _Getdays();
  CHECK(days && memcmp(days, ":Sun:Sunday:Mon:Monday", 22) == 0);
  free(days);
  char* months = _Getmonths();
  CHECK(months && memcmp(months, ":Jan:January:Feb:February", 25) == 0);
  free(months);

  /* ---- files: binary write, text-mode read (CRLF -> LF) ---- */
  void* f = fopen("T:\\crt_test.txt", "wb");
  CHECK(f != 0);
  CHECK(fwrite("line1\r\nline2\n", 1, 13, f) == 13);
  fclose(f);
  f = fopen("t:/CRT_TEST.TXT", "r");
  CHECK(f != 0);
  CHECK(fgets(buf, sizeof buf, f) && gstreq(buf, "line1\n"));
  CHECK(fgets(buf, sizeof buf, f) && gstreq(buf, "line2\n"));
  CHECK(fgets(buf, sizeof buf, f) == 0);
  fclose(f);
  uint8_t st[56];
  CHECK(_stat64("T:\\crt_test.txt", st) == 0);
  int64_t size;
  memcpy(&size, st + 24, 8);
  CHECK(size == 13);
  CHECK(_access("T:\\missing.txt", 0) == -1 && *_errno() == 2);
  CHECK(fopen("G:\\crt.exe", "wb") == 0 && *_errno() == 13); /* package is read-only */

  /* ---- environment shared with kernelx ---- */
  CHECK(getenv("XBEM_CRT_TEST") == 0);
  CHECK(_putenv_s("XBEM_CRT_TEST", "on") == 0);
  CHECK(getenv("xbem_crt_test") && gstreq(getenv("xbem_crt_test"), "on"));
  CHECK(GetEnvironmentVariableA("XBEM_CRT_TEST", buf, sizeof buf) == 2 && gstreq(buf, "on"));

  return (DWORD)finish("crt");
}
