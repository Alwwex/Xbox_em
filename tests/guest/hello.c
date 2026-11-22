/* Basic loader + kernel HLE smoke test. */
#include "guest.h"

extern char __ImageBase; /* provided by the linker: start of this image */

DWORD guest_main(void* peb) {
  say("Hello from guest code running under xbem");
  CHECK(peb != 0);

  /* Time. */
  int64_t freq = 0, c0 = 0, c1 = 0;
  CHECK(QueryPerformanceFrequency(&freq) && freq == 10000000);
  CHECK(QueryPerformanceCounter(&c0));
  uint64_t t0 = GetTickCount64();
  Sleep(20);
  uint64_t t1 = GetTickCount64();
  CHECK(QueryPerformanceCounter(&c1));
  CHECK(t1 - t0 >= 15);
  CHECK(c1 > c0);

  /* Last error lives in the emulated TEB. */
  SetLastError(1234);
  CHECK(GetLastError() == 1234);
  uint64_t teb_last_error;
  __asm__ volatile("movl %%gs:0x68, %k0" : "=r"(teb_last_error));
  CHECK((uint32_t)teb_last_error == 1234);

  /* TEB self pointer and thread id. */
  uint64_t teb_self, teb_tid;
  __asm__ volatile("movq %%gs:0x30, %0" : "=r"(teb_self));
  __asm__ volatile("movq %%gs:0x48, %0" : "=r"(teb_tid));
  CHECK(teb_self != 0);
  CHECK(teb_tid == GetCurrentThreadId());

  /* Heap. */
  HANDLE heap = GetProcessHeap();
  unsigned char* p = (unsigned char*)HeapAlloc(heap, HEAP_ZERO_MEMORY, 4096);
  CHECK(p != 0);
  CHECK(p[0] == 0 && p[4095] == 0);
  CHECK(((uintptr_t)p & 15) == 0);
  p[100] = 7;
  CHECK(HeapFree(heap, 0, p));

  /* Virtual memory: reserve, then commit part of it. */
  unsigned char* r = (unsigned char*)VirtualAlloc(0, 1 << 20, MEM_RESERVE, PAGE_READWRITE);
  CHECK(r != 0 && ((uintptr_t)r & 0xFFFF) == 0);
  unsigned char* c = (unsigned char*)VirtualAlloc(r + 0x3000, 0x2000, MEM_COMMIT, PAGE_READWRITE);
  CHECK(c == r + 0x3000);
  c[0] = 1;
  c[0x1FFF] = 2;
  CHECK(VirtualFree(r, 0, MEM_RELEASE));

  /* Modules. */
  CHECK(GetModuleHandleA(0) == (HANDLE)&__ImageBase);
  HANDLE kx = GetModuleHandleA("kernelx.dll");
  CHECK(kx != 0);
  CHECK(GetProcAddress(kx, "GetTickCount64") == (void*)&GetTickCount64);
  CHECK(GetProcAddress(kx, "NoSuchFunction") == 0);
  char path[260];
  DWORD n = GetModuleFileNameA(0, path, sizeof path);
  CHECK(n > 3 && path[1] == ':' && path[2] == '\\');
  say(path);

  /* Strings. */
  WCHAR wide[16];
  int len = MultiByteToWideChar(65001, 0, "h\xC3\xA9llo", -1, wide, 16);
  CHECK(len == 6);
  CHECK(wide[1] == 0xE9 && wide[5] == 0);

  return (DWORD)finish("hello");
}
