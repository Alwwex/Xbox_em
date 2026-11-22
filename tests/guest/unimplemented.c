/* Calls an import xbem does not implement. Without --strict the stub returns
 * 0 with ERROR_CALL_NOT_IMPLEMENTED; with --strict the process stops. */
#include "guest.h"

DWORD guest_main(void* peb) {
  (void)peb;
  SetLastError(0);
  CHECK(XbemDefinitelyMissing() == 0);
  CHECK(GetLastError() == 120); /* ERROR_CALL_NOT_IMPLEMENTED */
  XbemDefinitelyMissing();
  return (DWORD)finish("unimplemented");
}
