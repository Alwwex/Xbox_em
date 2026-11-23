// HLE entry points of the console Direct3D 12 runtime (D3D12.X).
//
// Device creation goes through the GPU backend seam (src/gpu). With the null
// backend every entry point fails cleanly with DXGI_ERROR_UNSUPPORTED /
// E_NOTIMPL, which lets a title reach - and report - its graphics init.

#include "gpu/backend.h"
#include "gpu/root_signature.h"
#include "hle/d3d12_device.h"
#include "hle/discovery.h"
#include "hle/hle_util.h"
#include "platform/memory.h"

#include <atomic>
#include <string>
#include <vector>

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

// Root signature serialization: the description is parsed from guest memory
// and serialized as DXBC/RTS0, the format the PC runtime produces; the device
// (CreateRootSignature) and the deserializer read it back.
HRESULT serialize_root_signature(const char* function, uint64_t desc, uint32_t version, void** blob,
                                 void** error_blob) {
  if (error_blob) *error_blob = nullptr;
  if (!blob) return E_POINTER;
  *blob = nullptr;
  gpu::RootSignature rs;
  std::string error;
  const gpu::MemoryReader read = [](uint64_t a, void* out, size_t n) {
    return a >= 0x10000 && a < 0x800000000000ull && platform::safe_read(a, out, n);
  };
  if (!read_root_signature_desc(read, desc, version, rs, error)) {
    XBEM_WARN("d3d12", "{}: {} -> E_INVALIDARG", function, error);
    if (error_blob) *error_blob = make_d3d_blob(std::vector<uint8_t>(error.begin(), error.end() + 1));
    return E_INVALIDARG;
  }
  std::vector<uint8_t> bytes = gpu::serialize_root_signature(rs);
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 16) XBEM_INFO("d3d12", "{}: {} ({} bytes)", function, rs.summary(), bytes.size());
  *blob = make_d3d_blob(std::move(bytes));
  return S_OK;
}

// HRESULT D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**)
HRESULT XBEM_GUEST_ABI hle_D3D12SerializeRootSignature(const void* desc, uint32_t version, void** blob,
                                                       void** error_blob) {
  return serialize_root_signature("D3D12SerializeRootSignature", reinterpret_cast<uint64_t>(desc),
                                  version ? version : 1, blob, error_blob);
}

// HRESULT D3D12SerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**)
HRESULT XBEM_GUEST_ABI hle_D3D12SerializeVersionedRootSignature(const void* desc, void** blob, void** error_blob) {
  return serialize_root_signature("D3D12SerializeVersionedRootSignature", reinterpret_cast<uint64_t>(desc), 0, blob,
                                  error_blob);
}

// HRESULT D3D10CreateBlob(SIZE_T size, ID3DBlob**)
HRESULT XBEM_GUEST_ABI hle_D3D10CreateBlob(SIZE_T size, void** blob) {
  if (!blob) return E_POINTER;
  *blob = make_d3d_blob(std::vector<uint8_t>(size));
  return S_OK;
}

// Reserves/configures the GPU virtual address space before device creation.
// Nothing to configure with host memory; report success (titles proceed).
HRESULT XBEM_GUEST_ABI hle_D3D12ConfigureVirtualMemoryX(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
  XBEM_LOG_CALL("d3d12", "S_OK", a, b, c, d);
  return S_OK;
}

// HRESULT D3D12CreateRootSignatureDeserializer(const void* data, SIZE_T size, REFIID, void** out)
HRESULT XBEM_GUEST_ABI hle_D3D12CreateRootSignatureDeserializer(const void* data, SIZE_T size, const void*,
                                                                void** out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  std::vector<uint8_t> bytes(std::min<SIZE_T>(size, 1 << 20));
  gpu::RootSignature rs;
  std::string error;
  if (!data || !platform::safe_read(reinterpret_cast<uint64_t>(data), bytes.data(), bytes.size()) ||
      !gpu::deserialize_root_signature(bytes.data(), bytes.size(), rs, error)) {
    XBEM_WARN("d3d12", "D3D12CreateRootSignatureDeserializer({} bytes): {} -> E_INVALIDARG", size, error);
    return E_INVALIDARG;
  }
  *out = make_root_signature_deserializer(rs);
  return S_OK;
}

}  // namespace

ModuleDef make_d3d12_module() {
  ModuleDef m;
  m.name = "d3d12_xs.dll";  // Xbox Series flavour of D3D12.X
  m.aliases = {"d3d12_x.dll", "d3d12.dll"};
  m.description = "Direct3D 12 (D3D12.X): device creation routed to the GPU backend (null for now)";
  m.exports = {
      XBEM_HLE(D3D12XboxCreateDevice),        XBEM_HLE(D3D12CreateDevice),
      XBEM_HLE_STUB(D3D12GetDebugInterface),  XBEM_HLE(D3D12SerializeRootSignature),
      XBEM_HLE(D3D12SerializeVersionedRootSignature), XBEM_HLE(D3D12ConfigureVirtualMemoryX),
      XBEM_HLE(D3D12CreateRootSignatureDeserializer), XBEM_HLE(D3D10CreateBlob),
  };
  return m;
}

}  // namespace xbem::hle
