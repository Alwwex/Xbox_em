/* Calls an import xbem does not implement. Without --strict the stub returns
 * 0 with ERROR_CALL_NOT_IMPLEMENTED; with --strict the process stops. */
#include "guest.h"

DWORD guest_main(void* peb) {
  (void)peb;
  SetLastError(0);
  CHECK(XbemDefinitelyMissing() == 0);
  CHECK(GetLastError() == 120); /* ERROR_CALL_NOT_IMPLEMENTED */
  XbemDefinitelyMissing();
  /* The stub log shows the first four integer arguments and the call site. */
  CHECK(XbemMissingWithArgs(0x11, 0x22, 0x33, 0x44) == 0);
  return (DWORD)finish("unimplemented");
}
