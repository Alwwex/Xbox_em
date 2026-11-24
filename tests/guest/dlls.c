/* Static and dynamic loading of game DLLs, relocation, forwarders. */
#include "guest.h"

IMP int gamelib_attach_count(void);
IMP int gamelib_add(int a, int b);
IMP uint64_t gamelib_tick(void);

typedef int (*PLUGIN_FN)(void);

DWORD guest_main(void* peb) {
  (void)peb;
  CHECK(gamelib_attach_count() == 1);   /* DllMain ran exactly once, before the entry point */
  for (int i = 0; i < 3; ++i)
    CHECK(gamelib_add(1, 2) == 23);     /* relocated pointer inside the DLL works (3 calls: tracepoint test) */
  CHECK(gamelib_tick() > 0);            /* forwarder gamelib -> kernelx (HLE) */

  HANDLE lib = GetModuleHandleA("gamelib.dll");
  CHECK(lib != 0);
  CHECK((uintptr_t)lib != 0x140000000ull); /* it had to be relocated */

  HANDLE plugin = LoadLibraryA("PLUGIN.DLL"); /* case-insensitive lookup */
  CHECK(plugin != 0);
  PLUGIN_FN fn = (PLUGIN_FN)GetProcAddress(plugin, "plugin_value");
  CHECK(fn != 0);
  if (fn) CHECK(fn() == 1234);
  CHECK(LoadLibraryA("plugin.dll") == plugin);
  CHECK(LoadLibraryA("does_not_exist.dll") == 0);
  CHECK(GetLastError() == 126); /* ERROR_MOD_NOT_FOUND */

  return (DWORD)finish("dlls");
}
