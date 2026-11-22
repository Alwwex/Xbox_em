/* Faults on purpose so the crash report can be checked. */
#include "guest.h"

DWORD guest_main(void* peb) {
  (void)peb;
  say("about to crash");
  volatile int* bad = (volatile int*)(uintptr_t)0x10;
  *bad = 1;
  return 0;
}
