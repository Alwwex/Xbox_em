/* An exception nobody handles: xbem must report it and exit with its code. */
#include "guest.h"

IMP void RaiseException(DWORD code, DWORD flags, DWORD nargs, const uint64_t* args);

__attribute__((noinline)) static void deep(int n) {
  if (n == 0) {
    RaiseException(0xE0BADBADu, 1, 0, 0);
    return;
  }
  deep(n - 1);
  __asm__ volatile("nop");
}

DWORD guest_main(void* peb) {
  (void)peb;
  deep(3);
  say("not reached");
  return 0;
}
