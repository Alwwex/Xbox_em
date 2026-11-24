/* Executes int3 like __debugbreak(). Without options the run ends with a
 * crash report; with --continue-after-breakpoint it keeps going. */
#include "guest.h"

DWORD guest_main(void* peb) {
  (void)peb;
  say("before breakpoint");
  /* Like a title's assertion: the message in a register, loaded just before. */
  __asm__ volatile("int3" ::"c"("assertion: budget exceeded"));
  say("after breakpoint");
  return (DWORD)finish("breakpoint");
}
