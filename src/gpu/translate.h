#pragma once

// RDNA2 shader programs -> GLSL 4.60 for Vulkan.
//
// A console shader mixes scalar instructions (SALU/SMEM: uniform for a draw)
// and vector instructions (one lane per vertex or pixel). The scalar part is
// run on the CPU for every draw (ScalarRun below): it walks the program,
// loading descriptors and constants from guest memory, and every scalar value
// a vector instruction reads is captured into a per-draw array of 32-bit
// words ("slots"). The vector part becomes GLSL that reads those slots.
// Buffers are reached through buffer device addresses (the renderer copies
// the guest data a draw uses into GPU memory and puts the copy's address
// into the buffer's slot); textures and samplers are indices into bindless
// arrays.
//
// Supported: what the first screens of GTA VI use - straight-line programs
// (no branches; the exec mask is taken as all lanes), the common VOP1/VOP2/
// VOP3/VOP3P float and integer operations, VINTRP, MUBUF/MTBUF loads,
// image_sample/image_load, exports. Anything else makes translation fail
// with a reason naming the instruction.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "gpu/rdna2.h"

namespace xbem::gpu {

enum class ShaderStage { Vertex, Pixel };

// What the per-draw slots of a translated shader hold.
enum class SlotKind : uint8_t {
  Sgpr,     // 1 word: an SGPR's value at that point of the program
  Buffer,   // 4 words: device address (lo, hi), stride, size limit (records)
  Texture,  // 4 words: bindless 2D index, image info (width | height << 16), 2D array index, 3D index
  Sampler,  // 1 word: bindless sampler index
};

struct Slot {
  SlotKind kind = SlotKind::Sgpr;
  uint32_t instruction = 0;  // index into Program::instructions: captured when the scalar run reaches it
  uint32_t sgpr = 0;         // first SGPR (Buffer: 4, Texture: 8, Sampler: 4 consecutive)
  uint32_t offset = 0;       // first word in the per-draw array
};

struct DecodedInstruction {
  rdna2::Instruction insn;
  uint32_t pc = 0;  // byte offset in the program
};

struct Program {
  std::vector<DecodedInstruction> instructions;
};

struct TranslatedShader {
  ShaderStage stage = ShaderStage::Vertex;
  bool ok = false;
  std::string error;
  std::string glsl;
  std::vector<Slot> slots;
  uint32_t slot_words = 0;
  uint32_t outputs = 0;      // vertex: params exported (locations 0..outputs-1)
  uint32_t inputs = 0;       // pixel: attributes read (locations 0..inputs-1)
  uint32_t color_targets = 0;  // pixel: mrt exports (bit mask)
  Program program;
};

// Decodes `code` (a captured blob: three metadata words + s_inst_prefetch are
// skipped as instructions like any other) and translates it.
TranslatedShader translate_shader(const uint8_t* code, size_t size, ShaderStage stage);

// The scalar part of a draw. `read` fetches guest memory for SMEM loads
// (returns false for unreadable addresses: zeros are loaded); `resolve`
// turns the descriptor words found in SGPRs into slot words.
struct ScalarRun {
  uint32_t sgpr[128] = {};  // initial user SGPRs (s0..) - 106 SGPRs, vcc, m0, exec ...
  std::function<bool(uint64_t address, void* out, size_t size)> read;
  std::function<void(SlotKind kind, const uint32_t* words, uint32_t* out)> resolve;
  std::string error;
};

// Runs the scalar instructions of `shader` and fills `words` (slot_words
// entries). False with run.error when an instruction cannot run on the CPU.
bool run_scalar(const TranslatedShader& shader, ScalarRun& run, std::vector<uint32_t>& words);

}  // namespace xbem::gpu
