#pragma once

// RDNA2 (gfx10.3) shader instruction decoder and disassembler.
//
// The Xbox Series GPU executes AMD RDNA2 machine code, and console titles
// ship their shaders already compiled to it. Translating those shaders for a
// host GPU (to SPIR-V for Vulkan) starts here: splitting the binary into
// instructions, identifying each one and its operands.
//
// Opcode tables are generated from LLVM's gfx1030 disassembler
// (tools/gpu/gen_rdna2_tables.py) and the decoder is checked against it.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xbem::gpu::rdna2 {

enum class Format {
  Invalid,
  Sop2, Sopk, Sop1, Sopc, Sopp, Smem,
  Vop2, Vop1, Vopc, Vop3, Vop3p, Vintrp,
  Ds, Mubuf, Mtbuf, Mimg, Flat, Global, Scratch, Exp,
};

struct Instruction {
  Format format = Format::Invalid;
  uint32_t opcode = 0;
  const char* name = nullptr;  // nullptr: valid format but unknown opcode
  uint32_t size = 4;           // bytes, including literal / DPP / SDWA / NSA dwords
  uint32_t words[5] = {};      // raw dwords
  bool has_literal = false;
  uint32_t literal = 0;
  enum class Extension { None, Dpp16, Dpp8, Sdwa } extension = Extension::None;
};

const char* format_name(Format format);

// Decodes the instruction at `code` (at least 4 bytes available). Never reads
// past `available` bytes; a truncated instruction comes back as Invalid.
Instruction decode(const uint8_t* code, size_t available);

// One instruction in LLVM-like syntax. `pc` is used for branch targets.
std::string disassemble(const Instruction& insn, uint64_t pc);

struct Listing {
  std::string text;
  size_t instructions = 0;
  size_t unknown = 0;  // undecodable instructions
};

// Disassembles a shader program: from `code` until s_endpgm (inclusive) or
// the end of the buffer.
Listing disassemble_program(const uint8_t* code, size_t size, uint64_t base_address = 0, bool stop_at_endpgm = true);

}  // namespace xbem::gpu::rdna2
