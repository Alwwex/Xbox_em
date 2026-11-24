/* Faults on purpose so the crash report can be checked. The fault happens two
   calls deep so the report's unwind backtrace has guest frames to walk. */
#include "guest.h"

__attribute__((noinline)) static void fault_here(volatile int* p) { *p = 1; }

/* Exported, so tests can name it: run --trace-at crash.exe!call_fault */
__attribute__((noinline, dllexport)) void call_fault(volatile int* p) {
  fault_here(p);
  __asm__ volatile("" ::: "memory"); /* keeps the call from becoming a tail jump */
}

DWORD guest_main(void* peb) {
  (void)peb;
  say("about to crash");
  call_fault((volatile int*)(uintptr_t)0x10);
  return 0;
}
