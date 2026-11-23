#pragma once

#include <cstdint>
#include <vector>

namespace xbem::gpu {
struct RootSignature;
}

namespace xbem::hle {

// Signals a fence created by the headless device (ID3D12Fence); false when
// `fence` is not one.
bool signal_d3d12_fence(void* fence, uint64_t value);

// ID3DBlob / ID3D10Blob holding `bytes` (GetBufferPointer = slot 3, GetBufferSize = slot 4).
void* make_d3d_blob(std::vector<uint8_t> bytes);

// ID3D12RootSignatureDeserializer over a parsed root signature (slot 3: GetRootSignatureDesc).
void* make_root_signature_deserializer(const gpu::RootSignature& rs);

// The headless D3D12.X device (see d3d12_device.cpp): GPU work completes
// instantly, nothing is drawn, shader binaries can be captured.
// `iid`: the interface ID the title asked for (QueryInterface with it returns the device).
void* create_headless_d3d12_device(const void* iid);

}  // namespace xbem::hle
