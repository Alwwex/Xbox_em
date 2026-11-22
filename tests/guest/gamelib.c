/* A DLL shipped "with the game". Linked at the same preferred base as the
 * executable so the loader is forced to relocate it. */
#include "guest.h"

static volatile int g_attach_count;
static int g_table[4] = {10, 20, 30, 40};
/* volatile: the compiler must keep the absolute pointer, so the linker has to
 * emit a DIR64 base relocation for it. */
static int* volatile g_table_ptr = g_table;

BOOL DllMain(HANDLE module, DWORD reason, void* reserved) {
  (void)module;
  (void)reserved;
  if (reason == 1) ATOMIC_INC(&g_attach_count);
  return 1;
}

__declspec(dllexport) int gamelib_attach_count(void) { return g_attach_count; }
__declspec(dllexport) int gamelib_add(int a, int b) { return a + b + g_table_ptr[1]; }
