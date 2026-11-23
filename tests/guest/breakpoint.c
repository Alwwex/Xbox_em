/* Executes int3 like __debugbreak(). Without options the run ends with a
 * crash report; with --continue-after-breakpoint it keeps going. */
#include "guest.h"

DWORD guest_main(void* peb) {
  (void)peb;
  say("before breakpoint");
  __asm__ volatile("int3");
  say("after breakpoint");
  return (DWORD)finish("breakpoint");
}
