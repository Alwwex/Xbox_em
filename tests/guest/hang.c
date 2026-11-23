/* Deadlocks on purpose: the main thread and a worker both wait forever.
 * Used to test the Ctrl+C / Ctrl+\ guest thread dump. */
#include "guest.h"

static HANDLE g_never;

static DWORD worker(void* arg) {
  (void)arg;
  WaitForSingleObject(g_never, INFINITE);
  return 0;
}

DWORD guest_main(void* peb) {
  (void)peb;
  g_never = CreateEventW(0, 1, 0, 0);
  HANDLE t = CreateThread(0, 0, worker, 0, 0, 0);
  say("hang: waiting forever");
  WaitForSingleObject(t, INFINITE);
  return 0;
}
