// HLE entry points of the console Direct3D 12 runtime (D3D12.X).
//
// Device creation goes through the GPU backend seam (src/gpu). With the null
// backend every entry point fails cleanly with DXGI_ERROR_UNSUPPORTED /
// E_NOTIMPL, which lets a title reach - and report - its graphics init.

#include "gpu/backend.h"
#include "hle/d3d12_device.h"
#include "hle/discovery.h"
#include "hle/hle_util.h"

namespace xbem::hle {
namespace {

using namespace win;


// HRESULT D3D12XboxCreateDevice(IGraphicsUnknown* adapter, const D3D12XBOX_CREATE_DEVICE_PARAMETERS*, REFIID, void**)
// Without a rendering backend the title gets the headless device: GPU work
// completes instantly and nothing is drawn, so its CPU side keeps running.
HRESULT headless_device(const char* function, const void* params, const void* iid, void** device) {
  const gpu::BackendInfo info = gpu::active_backend().info();
  XBEM_WARN("d3d12", "{}: GPU backend '{}' cannot render - using the HEADLESS device (GPU work completes "
            "instantly, nothing is drawn)", function, info.name);
  if (params) XBEM_INFO("d3d12", "{} parameters: {}", function, describe_arg(reinterpret_cast<uint64_t>(params)));
  if (!device) return E_POINTER;
  *device = create_headless_d3d12_device(iid);
  return S_OK;
}

HRESULT XBEM_GUEST_ABI hle_D3D12XboxCreateDevice(void*, const void* params, const void* iid, void** device) {
  return headless_device("D3D12XboxCreateDevice", params, iid, device);
}

// HRESULT D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL, REFIID, void**)
HRESULT XBEM_GUEST_ABI hle_D3D12CreateDevice(void*, uint32_t, const void* iid, void** device) {
  return headless_device("D3D12CreateDevice", nullptr, iid, device);
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

// Reserves/configures the GPU virtual address space before device creation.
// Nothing to configure with host memory; report success (titles proceed).
HRESULT XBEM_GUEST_ABI hle_D3D12ConfigureVirtualMemoryX(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
  XBEM_LOG_CALL("d3d12", "S_OK", a, b, c, d);
  return S_OK;
}

// HRESULT D3D12CreateRootSignatureDeserializer(const void* data, SIZE_T size, REFIID, void** out)
HRESULT XBEM_GUEST_ABI hle_D3D12CreateRootSignatureDeserializer(const void*, SIZE_T, const void*, void** out) {
  if (out) *out = nullptr;
  XBEM_WARN_ONCE("d3d12", "D3D12CreateRootSignatureDeserializer not implemented -> E_NOTIMPL");
  return E_NOTIMPL;
}

}  // namespace

ModuleDef make_d3d12_module() {
  ModuleDef m;
  m.name = "d3d12_xs.dll";  // Xbox Series flavour of D3D12.X
  m.aliases = {"d3d12_x.dll", "d3d12.dll"};
  m.description = "Direct3D 12 (D3D12.X): device creation routed to the GPU backend (null for now)";
  m.exports = {
      XBEM_HLE(D3D12XboxCreateDevice),        XBEM_HLE(D3D12CreateDevice),
      XBEM_HLE_STUB(D3D12GetDebugInterface),  XBEM_HLE_STUB(D3D12SerializeRootSignature),
      XBEM_HLE_STUB(D3D12SerializeVersionedRootSignature), XBEM_HLE(D3D12ConfigureVirtualMemoryX),
      XBEM_HLE_STUB(D3D12CreateRootSignatureDeserializer),
  };
  return m;
}

}  // namespace xbem::hle
