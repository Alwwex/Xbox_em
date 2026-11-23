/* Implicit (static) TLS, the way MSVC compiles __declspec(thread):
 *   mov rax, gs:[0x58]      ; TEB->ThreadLocalStoragePointer
 *   mov ecx, [_tls_index]   ; written by the loader
 *   mov rax, [rax + rcx*8]  ; this module's TLS block
 * plus TLS callbacks. With -nostdlib we provide the TLS directory ourselves
 * (normally the CRT's tlssup.c does that). */
#include "guest.h"

typedef void (*TLS_CALLBACK)(void*, DWORD, void*);

unsigned int _tls_index = 0xFFFFFFFFu;
__attribute__((section(".tls$AAA"), used)) char _tls_start = 0;
__attribute__((section(".tls$ZZZ"), used)) char _tls_end = 0;
__attribute__((section(".tls$"), used)) int tls_counter = 7;
__attribute__((section(".tls$"), used)) int64_t tls_big = 0x1122334455667788LL;

static volatile int g_process_attach, g_thread_attach, g_thread_detach;

static void tls_callback(void* module, DWORD reason, void* reserved) {
  (void)module;
  (void)reserved;
  if (reason == 1) ATOMIC_INC(&g_process_attach);
  if (reason == 2) ATOMIC_INC(&g_thread_attach);
  if (reason == 3) ATOMIC_INC(&g_thread_detach);
}

__attribute__((section(".CRT$XLA"), used)) TLS_CALLBACK __xl_a = 0;
__attribute__((section(".CRT$XLB"), used)) TLS_CALLBACK __xl_b = tls_callback;
__attribute__((section(".CRT$XLZ"), used)) TLS_CALLBACK __xl_z = 0;

typedef struct {
  uint64_t start, end, index, callbacks;
  uint32_t zero_fill, characteristics;
} TlsDirectory;

__attribute__((section(".rdata$T"), used)) const TlsDirectory _tls_used = {
    (uint64_t)&_tls_start, (uint64_t)&_tls_end, (uint64_t)&_tls_index, (uint64_t)(&__xl_a + 1), 0, 0};

static char* tls_block(void) {
  char** array;
  __asm__ volatile("movq %%gs:0x58, %0" : "=r"(array));
  return array[_tls_index];
}

#define TLS(var) (*(__typeof__(var)*)(tls_block() + ((char*)&(var) - (char*)&_tls_start)))

IMP int tlsdll_value(void);
IMP unsigned tlsdll_index(void);

static volatile int g_errors;

static DWORD worker(void* arg) {
  const int id = (int)(intptr_t)arg;
  if (TLS(tls_counter) != 7) ATOMIC_INC(&g_errors);              /* fresh copy of the template */
  if (TLS(tls_big) != 0x1122334455667788LL) ATOMIC_INC(&g_errors);
  TLS(tls_counter) = 1000 + id;
  Sleep(20);
  if (TLS(tls_counter) != 1000 + id) ATOMIC_INC(&g_errors);      /* not clobbered by others */
  return 0;
}

DWORD guest_main(void* peb) {
  (void)peb;
  /* The exe owns TLS index 0 even though tlsdll.dll was initialised first:
   * MSVC code in an .exe reads ThreadLocalStoragePointer[0] directly. */
  CHECK(_tls_index == 0);
  CHECK(tlsdll_index() != 0);
  CHECK(tlsdll_value() == 1234);
  CHECK(g_process_attach == 1);
  CHECK(TLS(tls_counter) == 7);
  TLS(tls_counter) = 42;

  HANDLE t[3];
  for (int i = 0; i < 3; ++i) t[i] = CreateThread(0, 0, worker, (void*)(intptr_t)i, 0, 0);
  CHECK(WaitForMultipleObjects(3, t, 1, 10000) == WAIT_OBJECT_0);
  for (int i = 0; i < 3; ++i) CloseHandle(t[i]);

  CHECK(g_errors == 0);
  CHECK(TLS(tls_counter) == 42);
  CHECK(tls_counter == 7); /* the template itself is never written */
  CHECK(ATOMIC_LOAD(&g_thread_attach) == 3);
  CHECK(ATOMIC_LOAD(&g_thread_detach) == 3);
  return (DWORD)finish("tls");
}
