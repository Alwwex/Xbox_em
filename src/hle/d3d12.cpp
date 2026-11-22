// HLE entry points of the console Direct3D 12 runtime (D3D12.X).
//
// Device creation goes through the GPU backend seam (src/gpu). With the null
// backend every entry point fails cleanly with DXGI_ERROR_UNSUPPORTED /
// E_NOTIMPL, which lets a title reach - and report - its graphics init.

#include "gpu/backend.h"
#include "hle/hle_util.h"

namespace xbem::hle {
namespace {

using namespace win;

HRESULT device_unavailable(const char* function) {
  const gpu::BackendInfo info = gpu::active_backend().info();
  XBEM_WARN("d3d12", "{}: GPU backend '{}' cannot create devices -> DXGI_ERROR_UNSUPPORTED ({})", function, info.name,
            info.notes);
  return DXGI_ERROR_UNSUPPORTED;
}

// HRESULT D3D12XboxCreateDevice(IGraphicsUnknown* adapter, const D3D12XBOX_CREATE_DEVICE_PARAMETERS*, REFIID, void**)
HRESULT XBEM_GUEST_ABI hle_D3D12XboxCreateDevice(void*, const void*, const void*, void** device) {
  if (device) *device = nullptr;
  return device_unavailable("D3D12XboxCreateDevice");
}

// HRESULT D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL, REFIID, void**)
HRESULT XBEM_GUEST_ABI hle_D3D12CreateDevice(void*, uint32_t, const void*, void** device) {
  if (device) *device = nullptr;
  return device_unavailable("D3D12CreateDevice");
}

HRESULT XBEM_GUEST_ABI hle_D3D12GetDebugInterface(const void*, void** out) {
  if (out) *out = nullptr;
  return E_NOTIMPL;
}

HRESULT XBEM_GUEST_ABI hle_D3D12SerializeRootSignature(const void*, uint32_t, void** blob, void** error_blob) {
  if (blob) *blob = nullptr;
  if (error_blob) *error_blob = nullptr;
  XBEM_WARN_ONCE("d3d12", "D3D12SerializeRootSignature not implemented -> E_NOTIMPL");
  return E_NOTIMPL;
}

HRESULT XBEM_GUEST_ABI hle_D3D12SerializeVersionedRootSignature(const void*, void** blob, void** error_blob) {
  return hle_D3D12SerializeRootSignature(nullptr, 0, blob, error_blob);
}

}  // namespace

ModuleDef make_d3d12_module() {
  ModuleDef m;
  m.name = "d3d12_xs.dll";  // Xbox Series flavour of D3D12.X
  m.aliases = {"d3d12_x.dll", "d3d12.dll"};
  m.description = "Direct3D 12 (D3D12.X): device creation routed to the GPU backend (null for now)";
  m.exports = {
      XBEM_HLE_STUB(D3D12XboxCreateDevice),   XBEM_HLE_STUB(D3D12CreateDevice),
      XBEM_HLE_STUB(D3D12GetDebugInterface),  XBEM_HLE_STUB(D3D12SerializeRootSignature),
      XBEM_HLE_STUB(D3D12SerializeVersionedRootSignature),
  };
  return m;
}

}  // namespace xbem::hle
