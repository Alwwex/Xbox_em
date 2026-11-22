/* Loaded at run time with LoadLibraryA. */
#include "guest.h"

static volatile int g_initialised;

BOOL DllMain(HANDLE module, DWORD reason, void* reserved) {
  (void)module;
  (void)reserved;
  if (reason == 1) g_initialised = 1234;
  return 1;
}

__declspec(dllexport) int plugin_value(void) { return g_initialised; }
