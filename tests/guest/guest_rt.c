/* Tiny freestanding runtime for the guest test programs. */
#include "guest.h"

/* The compiler may emit calls to these even with -ffreestanding. */
void* memset(void* dst, int c, size_t n) {
  unsigned char* p = (unsigned char*)dst;
  while (n--) *p++ = (unsigned char)c;
  return dst;
}

void* memcpy(void* dst, const void* src, size_t n) {
  unsigned char* d = (unsigned char*)dst;
  const unsigned char* s = (const unsigned char*)src;
  while (n--) *d++ = *s++;
  return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
  const unsigned char* x = (const unsigned char*)a;
  const unsigned char* y = (const unsigned char*)b;
  for (; n; --n, ++x, ++y)
    if (*x != *y) return *x < *y ? -1 : 1;
  return 0;
}

size_t gstrlen(const char* s) {
  size_t n = 0;
  while (s[n]) ++n;
  return n;
}

int gstreq(const char* a, const char* b) {
  while (*a && *a == *b) ++a, ++b;
  return *a == *b;
}

static int g_failures;

void say(const char* text) { OutputDebugStringA(text); }

void say_num(const char* label, int64_t value) {
  char buf[96];
  size_t n = 0;
  while (label[n] && n < 60) {
    buf[n] = label[n];
    ++n;
  }
  buf[n++] = '=';
  char digits[24];
  int d = 0;
  uint64_t v = value < 0 ? (uint64_t)(-value) : (uint64_t)value;
  do {
    digits[d++] = (char)('0' + v % 10);
    v /= 10;
  } while (v);
  if (value < 0) buf[n++] = '-';
  while (d) buf[n++] = digits[--d];
  buf[n] = 0;
  say(buf);
}

void check_failed(int line, const char* expr) {
  ++g_failures;
  say_num("CHECK FAILED at line", line);
  say(expr);
}

int finish(const char* test_name) {
  char buf[96] = "PASS ";
  if (g_failures) {
    say_num("failures", g_failures);
    memcpy(buf, "FAIL ", 5);
  }
  size_t n = gstrlen(test_name);
  if (n > 80) n = 80;
  memcpy(buf + 5, test_name, n);
  buf[5 + n] = 0;
  say(buf);
  return g_failures ? 1 : 0;
}
