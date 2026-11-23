// Console-only hardware APIs that have no public documentation:
//
//   acphal.dll  - audio co-processor HAL (APU memory heaps, shape contexts)
//   chad.dll    - audio DSP command interface (XDsp*)
//   xapu.dll    - audio processing unit command queues (XApu*)
//   xcrdapi.dll - the console DirectStorage runtime (DStorageGetFactory)
//   xg_xs.dll   - XG texture layout helpers for the Series GPU
//
// Until they are implemented, each entry point logs its first calls with all
// argument values (pointers are probed for GUIDs and strings) and the call
// site, and fails in the way the caller checks for (E_NOTIMPL for HRESULT
// callers, otherwise 0/NULL). The logs are the evidence used to implement them.

#include "hle/discovery.h"
#include "hle/hle_util.h"

namespace xbem::hle {
namespace {

using namespace win;

XBEM_DISCOVERY_FAIL("acphal", AcpHalCreate)
XBEM_DISCOVERY_FAIL("acphal", AcpHalAllocateShapeContexts)
XBEM_DISCOVERY_FAIL("acphal", AcpHalReleaseShapeContexts)
XBEM_DISCOVERY_FAIL("acphal", ApuAlloc)
XBEM_DISCOVERY_FAIL("acphal", ApuCreateHeap)
XBEM_DISCOVERY_FAIL("acphal", ApuFree)
XBEM_DISCOVERY_FAIL("acphal", ApuIsVirtualAddressValid)
XBEM_DISCOVERY_FAIL("acphal", ApuMapApuAddress)
XBEM_DISCOVERY_FAIL("acphal", ApuMapVirtualAddress)

XBEM_DISCOVERY_FAIL("chad", XDspActivate)
XBEM_DISCOVERY_FAIL("chad", XDspConnectWithMaximumStreamLimit)
XBEM_DISCOVERY_FAIL("chad", XDspDeactivate)
XBEM_DISCOVERY_FAIL("chad", XDspDisconnect)
XBEM_DISCOVERY_FAIL("chad", XDspSubmitCommand)

XBEM_DISCOVERY_FAIL("xapu", XApuConnect)
XBEM_DISCOVERY_FAIL("xapu", XApuDequeueResult)
XBEM_DISCOVERY_FAIL("xapu", XApuDisconnect)
XBEM_DISCOVERY_FAIL("xapu", XApuEnqueueCommand)
XBEM_DISCOVERY_FAIL("xapu", XApuGetResultCount)

// HRESULT DStorageGetFactory(REFIID riid, void** factory)
HRESULT XBEM_GUEST_ABI hle_DStorageGetFactory(const void* iid, void** out) {
  XBEM_LOG_CALL("dstorage", "E_NOTIMPL (DirectStorage not emulated yet)", reinterpret_cast<uint64_t>(iid),
                reinterpret_cast<uint64_t>(out));
  if (out) *out = nullptr;
  return E_NOTIMPL;
}

// HRESULT XGCreateTextureComputer(const XG_RESOURCE_DESC*, XGTextureAddressComputer** out)
HRESULT XBEM_GUEST_ABI hle_XGCreateTextureComputer(const void* desc, void** out) {
  XBEM_LOG_CALL("xg", "E_NOTIMPL", reinterpret_cast<uint64_t>(desc), reinterpret_cast<uint64_t>(out));
  if (out) *out = nullptr;
  return E_NOTIMPL;
}
XBEM_DISCOVERY_FAIL("xg", XGSuggestSwizzleMode)

}  // namespace

std::vector<ModuleDef> make_console_modules() {
  std::vector<ModuleDef> out;
  auto add = [&](const char* name, const char* description, std::vector<ExportDef> exports) {
    ModuleDef m;
    m.name = name;
    m.description = description;
    m.exports = std::move(exports);
    out.push_back(std::move(m));
  };
  add("acphal.dll", "Audio co-processor HAL (not emulated: calls are logged and fail)",
      {XBEM_HLE_STUB(AcpHalCreate), XBEM_HLE_STUB(AcpHalAllocateShapeContexts),
       XBEM_HLE_STUB(AcpHalReleaseShapeContexts), XBEM_HLE_STUB(ApuAlloc), XBEM_HLE_STUB(ApuCreateHeap),
       XBEM_HLE_STUB(ApuFree), XBEM_HLE_STUB(ApuIsVirtualAddressValid), XBEM_HLE_STUB(ApuMapApuAddress),
       XBEM_HLE_STUB(ApuMapVirtualAddress)});
  add("chad.dll", "Audio DSP interface (not emulated: calls are logged and fail)",
      {XBEM_HLE_STUB(XDspActivate), XBEM_HLE_STUB(XDspConnectWithMaximumStreamLimit), XBEM_HLE_STUB(XDspDeactivate),
       XBEM_HLE_STUB(XDspDisconnect), XBEM_HLE_STUB(XDspSubmitCommand)});
  add("xapu.dll", "Audio processing unit queues (not emulated: calls are logged and fail)",
      {XBEM_HLE_STUB(XApuConnect), XBEM_HLE_STUB(XApuDequeueResult), XBEM_HLE_STUB(XApuDisconnect),
       XBEM_HLE_STUB(XApuEnqueueCommand), XBEM_HLE_STUB(XApuGetResultCount)});
  add("xcrdapi.dll", "Console DirectStorage runtime (not emulated yet)", {XBEM_HLE_STUB(DStorageGetFactory)});
  add("xg_xs.dll", "XG texture layout library (not emulated yet)",
      {XBEM_HLE_STUB(XGCreateTextureComputer), XBEM_HLE_STUB(XGSuggestSwizzleMode)});
  return out;
}

}  // namespace xbem::hle
