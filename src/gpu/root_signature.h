#pragma once

// D3D12 root signatures: the description structures a title builds
// (D3D12_VERSIONED_ROOT_SIGNATURE_DESC, versions 1.0 / 1.1 / 1.2), the
// serialized form (a DXBC container with an RTS0 part, as
// D3D12SerializeVersionedRootSignature produces it) and back.
//
// The GPU backend needs the parsed form to map root parameters onto the
// console shaders' user SGPRs and, later, onto Vulkan descriptor layouts.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace xbem::gpu {

struct RootSignature {
  enum class ParamType : uint32_t { DescriptorTable = 0, Constants32 = 1, Cbv = 2, Srv = 3, Uav = 4 };
  struct Range {
    uint32_t type = 0;  // D3D12_DESCRIPTOR_RANGE_TYPE: 0 SRV, 1 UAV, 2 CBV, 3 SAMPLER
    uint32_t count = 0;
    uint32_t base_register = 0;
    uint32_t space = 0;
    uint32_t flags = 0;   // 1.1+
    uint32_t offset = 0;  // OffsetInDescriptorsFromTableStart (0xFFFFFFFF = append)
  };
  struct Param {
    ParamType type = ParamType::DescriptorTable;
    uint32_t visibility = 0;  // D3D12_SHADER_VISIBILITY
    std::vector<Range> ranges;  // descriptor table
    uint32_t shader_register = 0, space = 0;
    uint32_t num_constants = 0;  // Constants32
    uint32_t flags = 0;          // root descriptor flags (1.1+)
  };
  uint32_t version = 1;  // 1 = 1.0, 2 = 1.1, 3 = 1.2
  uint32_t flags = 0;    // D3D12_ROOT_SIGNATURE_FLAGS (console builds set 0x03000000 bits)
  std::vector<Param> params;
  std::vector<std::array<uint32_t, 14>> static_samplers;  // D3D12_STATIC_SAMPLER_DESC1 words (Flags last)

  std::string summary() const;
};

// Reads guest memory; returns false when the range is not readable.
using MemoryReader = std::function<bool(uint64_t address, void* out, size_t size)>;

// Parses a D3D12_VERSIONED_ROOT_SIGNATURE_DESC at `address`. When
// `version_override` is non-zero the structure at `address` is a bare
// D3D12_ROOT_SIGNATURE_DESC of that version (D3D12SerializeRootSignature).
bool read_root_signature_desc(const MemoryReader& read, uint64_t address, uint32_t version_override,
                              RootSignature& out, std::string& error);

// The serialized blob: "DXBC" container with one RTS0 part.
std::vector<uint8_t> serialize_root_signature(const RootSignature& rs);

// Accepts a DXBC container holding RTS0, or a bare RTS0 part.
bool deserialize_root_signature(const uint8_t* data, size_t size, RootSignature& out, std::string& error);

// Builds D3D12_ROOT_SIGNATURE_DESC (1.0 layout, x64) with its parameter,
// range and sampler arrays in one buffer, for ID3D12RootSignatureDeserializer::
// GetRootSignatureDesc. Pointers inside are absolute: `buffer` must not move.
void build_root_signature_desc_1_0(const RootSignature& rs, std::vector<uint8_t>& buffer);

}  // namespace xbem::gpu
