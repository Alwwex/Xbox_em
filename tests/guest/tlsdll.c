/* A DLL with its own implicit TLS, loaded (and TLS-initialised) before the
 * executable that imports it. The executable must still get TLS index 0. */
#include "guest.h"

unsigned int _tls_index = 0xFFFFFFFFu;
__attribute__((section(".tls$AAA"), used)) char _tls_start = 0;
__attribute__((section(".tls$ZZZ"), used)) char _tls_end = 0;
__attribute__((section(".tls$"), used)) int dll_tls_value = 1234;

typedef struct {
  uint64_t start, end, index, callbacks;
  uint32_t zero_fill, characteristics;
} TlsDirectory;
__attribute__((section(".CRT$XLA"), used)) void* __xl_a = 0;
__attribute__((section(".CRT$XLZ"), used)) void* __xl_z = 0;
__attribute__((section(".rdata$T"), used)) const TlsDirectory _tls_used = {
    (uint64_t)&_tls_start, (uint64_t)&_tls_end, (uint64_t)&_tls_index, (uint64_t)(&__xl_a + 1), 0, 0};

__declspec(dllexport) int tlsdll_value(void) {
  char** array;
  __asm__ volatile("movq %%gs:0x58, %0" : "=r"(array));
  return *(int*)(array[_tls_index] + ((char*)&dll_tls_value - (char*)&_tls_start));
}
__declspec(dllexport) unsigned tlsdll_index(void) { return _tls_index; }

BOOL DllMain(HANDLE module, DWORD reason, void* reserved) {
  (void)module;
  (void)reason;
  (void)reserved;
  return 1;
}
