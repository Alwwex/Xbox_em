#pragma once

#include <cstdint>
#include <string>
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

// What the title has asked of the GPU so far (the run window shows it):
// frame events waited on, command lists created, draws and dispatches recorded.
// A console graphics pipeline (device slots 57 and 55, see docs/GPU.md):
// the parts of its description, copied at creation.
struct GraphicsPipeline {
  uint64_t root_signature = 0;  // +0x00 of the description
  std::vector<uint8_t> state_a, state_b;  // register state blobs
  std::vector<uint8_t> ps_code, ps_meta, vs_code, vs_meta;
  uint64_t vs_hash = 0, ps_hash = 0;  // as named by --dump-shaders
  uint64_t part_hashes[6] = {};       // state a, state b, ps, ps meta, vs, vs meta
  bool blend_override = false;        // created by slot 55
  // Device slot 86 (a PC pipeline stream with DXIL): GTA VI's only such
  // pipeline is VS_Clear/PS_Clear - a full-target fill with a colour from
  // root parameter 0 - which the renderer performs as a clear.
  bool stream_clear = false;
  uint8_t blend[0x148] = {};          // its D3D12_BLEND_DESC
  // Registers found in the console state (state_a): CB_BLEND0-7_CONTROL,
  // CB_TARGET_MASK follows them (0xffffffff marks the block) and
  // DB_DEPTH_CONTROL is 6 dwords before (GPU.md "pipeline state blobs").
  bool has_registers = false;
  uint32_t cb_blend[8] = {};
  uint32_t db_depth_control = 0;
};

// A console compute pipeline (device slot 67, DeserializeComputePipelineStateX):
// root signature (+0x00), register state (+0x08: launch state, 0x1c bytes),
// more state (+0x18), RDNA2 code (+0x28), metadata (+0x38).
struct ComputePipeline {
  uint64_t root_signature = 0;
  std::vector<uint8_t> state_a, state_b, code, meta;
  uint64_t cs_hash = 0;
};

struct D3d12Counters {
  uint64_t frames, command_lists, draws, dispatches;
};
D3d12Counters d3d12_counters();
// Fences someone waits on (SetEventOnCompletion) whose value is not reached
// yet: "name at value waited for N (k waiters); ..." - empty when none.
std::string d3d12_wait_report();
// DirectStorage queues with work not run yet and incomplete status entries
// (hle/dstorage.cpp) - empty when none.
std::string dstorage_report();
// Bytes the title read with ReadFile (for the status line).
void count_readfile_bytes(uint64_t n);

}  // namespace xbem::hle
