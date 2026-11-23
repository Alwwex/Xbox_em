#include "gpu/rdna2.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <string_view>

namespace xbem::gpu::rdna2 {
namespace {

#include "gpu/rdna2_opcodes.inc"

uint32_t bits(uint32_t w, int hi, int lo) { return (w >> lo) & ((1u << (hi - lo + 1)) - 1); }

template <size_t N>
const char* lookup(const char* const (&table)[N], uint32_t op) {
  return op < N ? table[op] : nullptr;
}

bool contains(const char* s, std::string_view part) { return s && std::string_view(s).find(part) != std::string_view::npos; }

// Register count of an operand type suffix ("b64" -> 2).
int type_regs(std::string_view t) {
  if (t.size() >= 3 && t.substr(t.size() - 2) == "64") return 2;
  if (t.ends_with("128")) return 4;
  return 1;
}

// For "v_cvt_f64_f32" returns {dst regs, src regs}; for single-type names both are equal.
std::pair<int, int> operand_regs(const char* name) {
  if (!name) return {1, 1};
  std::string_view n(name);
  std::vector<std::string_view> types;
  size_t pos = 0;
  while (pos < n.size()) {
    size_t next = n.find('_', pos);
    std::string_view part = n.substr(pos, next == std::string_view::npos ? std::string_view::npos : next - pos);
    if (part.size() >= 3 && (part[0] == 'b' || part[0] == 'u' || part[0] == 'i' || part[0] == 'f') &&
        part.substr(1).find_first_not_of("0123456789") == std::string_view::npos)
      types.push_back(part);
    if (next == std::string_view::npos) break;
    pos = next + 1;
  }
  if (types.empty()) return {1, 1};
  if (types.size() == 1) return {type_regs(types[0]), type_regs(types[0])};
  return {type_regs(types[0]), type_regs(types[1])};
}

std::string sgpr(uint32_t index, int regs) {
  if (regs == 1) return std::format("s{}", index);
  return std::format("s[{}:{}]", index, index + regs - 1);
}
std::string vgpr(uint32_t index, int regs) {
  if (regs == 1) return std::format("v{}", index);
  return std::format("v[{}:{}]", index, index + regs - 1);
}

// Scalar operand / 9-bit source operand (256+ are VGPRs).
std::string operand(uint32_t v, int regs, const Instruction& insn) {
  if (v >= 256) return vgpr(v - 256, regs);
  if (v <= 105) return sgpr(v, regs);
  switch (v) {
    case 106: return regs == 2 ? "vcc" : "vcc_lo";
    case 107: return "vcc_hi";
    case 124: return "m0";
    case 125: return "null";
    case 126: return regs == 2 ? "exec" : "exec_lo";
    case 127: return "exec_hi";
    case 128: return "0";
    case 235: return "src_shared_base";
    case 236: return "src_shared_limit";
    case 237: return "src_private_base";
    case 238: return "src_private_limit";
    case 239: return "src_pops_exiting_wave_id";
    case 240: return "0.5";
    case 241: return "-0.5";
    case 242: return "1.0";
    case 243: return "-1.0";
    case 244: return "2.0";
    case 245: return "-2.0";
    case 246: return "4.0";
    case 247: return "-4.0";
    case 248: return "0.15915494";
    case 250: return "dpp";
    case 251: return "src_vccz";
    case 252: return "src_execz";
    case 253: return "src_scc";
    case 254: return "src_lds_direct";
    case 255: return std::format("0x{:x}", insn.literal);
    default: break;
  }
  if (v >= 108 && v <= 123) return regs == 1 ? std::format("ttmp{}", v - 108) : std::format("ttmp[{}:{}]", v - 108, v - 108 + regs - 1);
  if (v >= 129 && v <= 192) return std::to_string(v - 128);
  if (v >= 193 && v <= 208) return std::to_string(-static_cast<int>(v - 192));
  return std::format("op{}", v);
}

bool scalar_has_literal(uint32_t ssrc) { return ssrc == 255; }

}  // namespace

const char* format_name(Format f) {
  switch (f) {
    case Format::Sop2: return "SOP2";
    case Format::Sopk: return "SOPK";
    case Format::Sop1: return "SOP1";
    case Format::Sopc: return "SOPC";
    case Format::Sopp: return "SOPP";
    case Format::Smem: return "SMEM";
    case Format::Vop2: return "VOP2";
    case Format::Vop1: return "VOP1";
    case Format::Vopc: return "VOPC";
    case Format::Vop3: return "VOP3";
    case Format::Vop3p: return "VOP3P";
    case Format::Vintrp: return "VINTRP";
    case Format::Ds: return "DS";
    case Format::Mubuf: return "MUBUF";
    case Format::Mtbuf: return "MTBUF";
    case Format::Mimg: return "MIMG";
    case Format::Flat: return "FLAT";
    case Format::Global: return "GLOBAL";
    case Format::Scratch: return "SCRATCH";
    case Format::Exp: return "EXP";
    case Format::Invalid: break;
  }
  return "INVALID";
}

Instruction decode(const uint8_t* code, size_t available) {
  Instruction in;
  if (available < 4) return in;
  uint32_t w;
  std::memcpy(&w, code, 4);
  in.words[0] = w;
  uint32_t base_size = 4;
  bool literal = false;

  auto vector_src0_extension = [&](uint32_t src0) {
    if (src0 == 255) literal = true;
    else if (src0 == 0xFA) in.extension = Instruction::Extension::Dpp16;
    else if (src0 == 0xE9 || src0 == 0xEA) in.extension = Instruction::Extension::Dpp8;
    else if (src0 == 0xF9) in.extension = Instruction::Extension::Sdwa;
  };

  if ((w >> 31) == 0) {
    const uint32_t src0 = bits(w, 8, 0);
    if (bits(w, 31, 25) == 0x3F) {
      in.format = Format::Vop1;
      in.opcode = bits(w, 16, 9);
      in.name = lookup(kVop1Names, in.opcode);
    } else if (bits(w, 31, 25) == 0x3E) {
      in.format = Format::Vopc;
      in.opcode = bits(w, 24, 17);
      in.name = lookup(kVopcNames, in.opcode);
    } else {
      in.format = Format::Vop2;
      in.opcode = bits(w, 30, 25);
      in.name = lookup(kVop2Names, in.opcode);
      if (contains(in.name, "mk_") || contains(in.name, "ak_")) literal = true;  // v_fmamk/v_madak...
    }
    vector_src0_extension(src0);
  } else {
    const uint32_t top6 = w >> 26;
    switch (top6) {
      case 0x35:
        in.format = Format::Vop3;
        in.opcode = bits(w, 25, 16);
        in.name = lookup(kVop3Names, in.opcode);
        base_size = 8;
        break;
      case 0x33:
        in.format = Format::Vop3p;
        in.opcode = bits(w, 22, 16);
        in.name = lookup(kVop3pNames, in.opcode);
        base_size = 8;
        break;
      case 0x32:
        in.format = Format::Vintrp;
        in.opcode = bits(w, 17, 16);
        in.name = lookup(kVintrpNames, in.opcode);
        break;
      case 0x36:
        in.format = Format::Ds;
        in.opcode = bits(w, 25, 18);
        in.name = lookup(kDsNames, in.opcode);
        base_size = 8;
        break;
      case 0x37: {
        const uint32_t seg = bits(w, 15, 14);
        in.opcode = bits(w, 24, 18);
        in.format = seg == 2 ? Format::Global : seg == 1 ? Format::Scratch : Format::Flat;
        in.name = seg == 2 ? lookup(kGlobalNames, in.opcode)
                  : seg == 1 ? lookup(kScratchNames, in.opcode)
                             : lookup(kFlatNames, in.opcode);
        base_size = 8;
        break;
      }
      case 0x38:
        in.format = Format::Mubuf;
        in.opcode = bits(w, 24, 18) | (bits(w, 25, 25) << 7);
        in.name = lookup(kMubufNames, in.opcode);
        base_size = 8;
        break;
      case 0x3A:
        in.format = Format::Mtbuf;
        base_size = 8;
        break;
      case 0x3C:
        in.format = Format::Mimg;
        in.opcode = bits(w, 24, 18) | (bits(w, 0, 0) << 7);
        in.name = lookup(kMimgNames, in.opcode);
        base_size = 8 + 4 * bits(w, 2, 1);  // NSA address dwords
        break;
      case 0x3D:
        in.format = Format::Smem;
        in.opcode = bits(w, 25, 18);
        in.name = lookup(kSmemNames, in.opcode);
        base_size = 8;
        break;
      case 0x3E:
        in.format = Format::Exp;
        in.name = "exp";
        base_size = 8;
        break;
      default:
        if ((w >> 30) == 0b10) {
          const uint32_t top9 = w >> 23;
          if (top9 == 0x17D) {
            in.format = Format::Sop1;
            in.opcode = bits(w, 15, 8);
            in.name = lookup(kSop1Names, in.opcode);
            literal = scalar_has_literal(bits(w, 7, 0));
          } else if (top9 == 0x17E) {
            in.format = Format::Sopc;
            in.opcode = bits(w, 22, 16);
            in.name = lookup(kSopcNames, in.opcode);
            literal = scalar_has_literal(bits(w, 7, 0)) || scalar_has_literal(bits(w, 15, 8));
          } else if (top9 == 0x17F) {
            in.format = Format::Sopp;
            in.opcode = bits(w, 22, 16);
            in.name = lookup(kSoppNames, in.opcode);
          } else if ((w >> 28) == 0xB) {
            in.format = Format::Sopk;
            in.opcode = bits(w, 27, 23);
            in.name = lookup(kSopkNames, in.opcode);
            literal = contains(in.name, "imm32");
          } else {
            in.format = Format::Sop2;
            in.opcode = bits(w, 29, 23);
            in.name = lookup(kSop2Names, in.opcode);
            literal = scalar_has_literal(bits(w, 7, 0)) || scalar_has_literal(bits(w, 15, 8));
          }
        } else {
          return Instruction{};
        }
    }
  }

  if (base_size >= 8) {
    if (available < 8) return Instruction{};
    std::memcpy(&in.words[1], code + 4, 4);
  }
  if (in.format == Format::Mtbuf) {
    in.opcode = bits(w, 18, 16) | (bits(in.words[1], 21, 21) << 3);
    in.name = lookup(kMtbufNames, in.opcode);
  }
  if (in.format == Format::Vop3 || in.format == Format::Vop3p) {
    const uint32_t w1 = in.words[1];
    literal = bits(w1, 8, 0) == 255 || bits(w1, 17, 9) == 255 || bits(w1, 26, 18) == 255;
  }
  for (uint32_t k = 2; k * 4 < base_size; ++k) {
    if (available < (k + 1) * 4) return Instruction{};
    std::memcpy(&in.words[k], code + 4 * k, 4);
  }
  in.size = base_size;
  if (literal || in.extension != Instruction::Extension::None) {
    if (available < in.size + 4) return Instruction{};
    uint32_t extra;
    std::memcpy(&extra, code + in.size, 4);
    in.words[in.size / 4] = extra;
    if (literal) {
      in.has_literal = true;
      in.literal = extra;
    }
    in.size += 4;
  }
  return in;
}

std::string disassemble(const Instruction& in, uint64_t pc) {
  if (in.format == Format::Invalid) return std::format(".long 0x{:08x}", in.words[0]);
  if (!in.name) return std::format("; unknown {} opcode {} ({:08x})", format_name(in.format), in.opcode, in.words[0]);
  const uint32_t w = in.words[0], w1 = in.words[1];
  const auto [dst_regs, src_regs] = operand_regs(in.name);
  std::string ops;
  auto add = [&](const std::string& s) {
    if (!ops.empty()) ops += ", ";
    ops += s;
  };
  std::string_view name(in.name);

  switch (in.format) {
    case Format::Sop2:
      add(operand(bits(w, 22, 16), dst_regs, in));
      add(operand(bits(w, 7, 0), src_regs, in));
      add(operand(bits(w, 15, 8), src_regs, in));
      break;
    case Format::Sopk:
      add(operand(bits(w, 22, 16), dst_regs, in));
      add(std::format("0x{:x}", bits(w, 15, 0)));
      if (in.has_literal) add(std::format("0x{:x}", in.literal));
      break;
    case Format::Sop1:
      if (bits(w, 22, 16) != 0 || !name.starts_with("s_set")) add(operand(bits(w, 22, 16), dst_regs, in));
      add(operand(bits(w, 7, 0), src_regs, in));
      break;
    case Format::Sopc:
      add(operand(bits(w, 7, 0), src_regs, in));
      add(operand(bits(w, 15, 8), src_regs, in));
      break;
    case Format::Sopp: {
      const int16_t imm = static_cast<int16_t>(bits(w, 15, 0));
      if (name.starts_with("s_branch") || name.starts_with("s_cbranch")) {
        add(std::format("0x{:x}", pc + 4 + static_cast<int64_t>(imm) * 4));
      } else if (name == "s_waitcnt") {
        const uint32_t v = bits(w, 15, 0);
        const uint32_t vm = (v & 0xF) | ((v >> 10) & 0x30), exp = (v >> 4) & 7, lgkm = (v >> 8) & 0x3F;
        std::string s;
        if (vm != 0x3F) s += std::format("vmcnt({}) ", vm);
        if (exp != 7) s += std::format("expcnt({}) ", exp);
        if (lgkm != 0x3F) s += std::format("lgkmcnt({}) ", lgkm);
        add(s.empty() ? std::string("0") : s.substr(0, s.size() - 1));
      } else if (name != "s_endpgm" && name != "s_barrier" && name != "s_wakeup" && name != "s_icache_inv" &&
                 name != "s_code_end" && name != "s_endpgm_saved" && name != "s_ttracedata") {
        if (imm || name == "s_nop") add(std::to_string(imm));
      }
      break;
    }
    case Format::Smem: {
      const uint32_t sdata = bits(w, 12, 6), sbase = bits(w, 5, 0) * 2;
      int count = 1;
      if (const size_t x = name.find("dwordx"); x != std::string_view::npos) count = std::atoi(in.name + x + 6);
      if (name.find("memtime") != std::string_view::npos || name.find("memrealtime") != std::string_view::npos) count = 2;
      const bool buffer = name.find("buffer") != std::string_view::npos;
      if (name.find("load") != std::string_view::npos || count == 2) add(sgpr(sdata, count));
      if (name.find("load") != std::string_view::npos) {
        add(sgpr(sbase, buffer ? 4 : 2));
        const int32_t offset = static_cast<int32_t>(bits(w1, 20, 0) << 11) >> 11;
        const uint32_t soffset = bits(w1, 31, 25);
        add(soffset != 0x7D ? operand(soffset, 1, in) : std::format("0x{:x}", offset));
      }
      break;
    }
    case Format::Vop2:
    case Format::Vop1:
    case Format::Vopc: {
      const uint32_t src0 = bits(w, 8, 0);
      if (in.format == Format::Vopc) add("vcc");
      else add(vgpr(bits(w, 24, 17), dst_regs));
      std::string s0 = in.extension == Instruction::Extension::None ? operand(src0, src_regs, in)
                                                                    : vgpr(bits(in.words[in.size / 4 - 1], 7, 0), src_regs);
      if (!(in.format == Format::Vop1 && (name == "v_nop" || name == "v_pipeflush" || name == "v_clrexcp"))) add(s0);
      if (in.format != Format::Vop1) {
        if (in.has_literal && in.format == Format::Vop2 && name.find("mk_") != std::string_view::npos)
          add(std::format("0x{:x}", in.literal));
        add(vgpr(bits(w, 16, 9), src_regs));
        if (in.has_literal && in.format == Format::Vop2 && name.find("ak_") != std::string_view::npos)
          add(std::format("0x{:x}", in.literal));
        if (name.starts_with("v_cndmask") || name.starts_with("v_add_co_ci") || name.starts_with("v_sub_co_ci") ||
            name.starts_with("v_subrev_co_ci"))
          add("vcc");
      }
      if (in.extension == Instruction::Extension::Dpp16) ops += std::format(" dpp_ctrl:0x{:x}", bits(in.words[1], 16, 8));
      if (in.extension == Instruction::Extension::Dpp8) ops += " dpp8";
      if (in.extension == Instruction::Extension::Sdwa) ops += " sdwa";
      break;
    }
    case Format::Vop3:
    case Format::Vop3p: {
      const uint32_t abs = bits(w, 10, 8), neg = bits(w1, 31, 29);
      auto src = [&](int i, uint32_t v) {
        std::string s = operand(v, src_regs, in);
        if (in.format == Format::Vop3 && (abs >> i) & 1) s = "|" + s + "|";
        if ((neg >> i) & 1) s = "-" + s;
        return s;
      };
      const bool compare = name.starts_with("v_cmp");
      add(compare ? operand(bits(w, 7, 0), 1, in) : vgpr(bits(w, 7, 0), dst_regs));
      const uint32_t s0 = bits(w1, 8, 0), s1 = bits(w1, 17, 9), s2 = bits(w1, 26, 18);
      add(src(0, s0));
      if (s1 || s2) add(src(1, s1));
      if (s2) add(src(2, s2));
      if (in.format == Format::Vop3 && bits(w, 15, 15)) ops += " clamp";
      break;
    }
    case Format::Vintrp:
      add(vgpr(bits(w, 25, 18), 1));
      add(vgpr(bits(w, 7, 0), 1));
      add(std::format("attr{}.{}", bits(w, 15, 10), "xyzw"[bits(w, 9, 8)]));
      break;
    case Format::Ds:
      add(vgpr(bits(w1, 31, 24), 1));
      add(vgpr(bits(w1, 7, 0), 1));
      add(vgpr(bits(w1, 15, 8), 1));
      if (bits(w, 15, 0)) ops += std::format(" offset:{}", bits(w, 15, 0));
      break;
    case Format::Mubuf:
    case Format::Mtbuf:
      add(vgpr(bits(w1, 15, 8), 1));
      add(vgpr(bits(w1, 7, 0), 1));
      add(sgpr(bits(w1, 20, 16) * 4, 4));
      add(operand(bits(w1, 31, 24), 1, in));
      if (bits(w, 12, 12)) ops += " offen";
      if (bits(w, 13, 13)) ops += " idxen";
      if (bits(w, 11, 0)) ops += std::format(" offset:{}", bits(w, 11, 0));
      if (in.format == Format::Mtbuf) ops += std::format(" format:{}", bits(w, 25, 19));
      break;
    case Format::Mimg:
      add(vgpr(bits(w1, 15, 8), std::max(1, static_cast<int>(std::popcount(bits(w, 11, 8))))));
      add(vgpr(bits(w1, 7, 0), 1));
      add(sgpr(bits(w1, 20, 16) * 4, 8));
      if (name.find("sample") != std::string_view::npos || name.find("gather") != std::string_view::npos)
        add(sgpr(bits(w1, 25, 21) * 4, 4));
      ops += std::format(" dmask:0x{:x} dim:{}", bits(w, 11, 8), bits(w, 5, 3));
      break;
    case Format::Flat:
    case Format::Global:
    case Format::Scratch: {
      const bool load = name.find("load") != std::string_view::npos || name.find("_rtn") != std::string_view::npos;
      if (load) add(vgpr(bits(w1, 31, 24), 1));
      add(vgpr(bits(w1, 7, 0), in.format == Format::Flat ? 2 : 1));
      if (!load || name.find("atomic") != std::string_view::npos) add(vgpr(bits(w1, 15, 8), 1));
      if (in.format != Format::Flat) {
        const uint32_t saddr = bits(w1, 22, 16);
        add(saddr == 0x7D ? std::string("off") : sgpr(saddr, 2));
      }
      const int32_t offset = static_cast<int32_t>(bits(w, 11, 0) << 20) >> 20;
      if (offset) ops += std::format(" offset:{}", offset);
      break;
    }
    case Format::Exp: {
      const uint32_t target = bits(w, 9, 4);
      std::string t = target <= 7     ? std::format("mrt{}", target)
                      : target == 8    ? "mrtz"
                      : target == 9    ? "null"
                      : target >= 12 && target <= 15 ? std::format("pos{}", target - 12)
                      : target >= 32 && target <= 63 ? std::format("param{}", target - 32)
                                                     : std::format("target{}", target);
      ops = t;
      for (int i = 0; i < 4; ++i) add((bits(w, 3, 0) >> i) & 1 ? vgpr(bits(w1, 8 * i + 7, 8 * i), 1) : std::string("off"));
      if (bits(w, 11, 11)) ops += " done";
      if (bits(w, 10, 10)) ops += " compr";
      if (bits(w, 12, 12)) ops += " vm";
      break;
    }
    case Format::Invalid: break;
  }
  if (in.format == Format::Exp) {  // "exp mrt0 v0, v1, ..." (no comma after the target)
    const size_t first = ops.find(", ");
    if (first != std::string::npos) ops.replace(first, 2, " ");
  }
  return ops.empty() ? std::string(in.name) : std::format("{} {}", in.name, ops);
}

Listing disassemble_program(const uint8_t* code, size_t size, uint64_t base, bool stop_at_endpgm) {
  Listing out;
  size_t pos = 0;
  while (pos + 4 <= size) {
    const Instruction in = decode(code + pos, size - pos);
    std::string bytes;
    const size_t n = in.format == Format::Invalid ? 4 : in.size;
    for (size_t i = 0; i < n / 4; ++i) {
      uint32_t word;
      std::memcpy(&word, code + pos + i * 4, 4);
      bytes += std::format("{:08x} ", word);
    }
    out.text += std::format("{:08x}: {:<36} {}\n", base + pos, bytes, disassemble(in, base + pos));
    ++out.instructions;
    if (in.format == Format::Invalid || !in.name) ++out.unknown;
    pos += n;
    if (stop_at_endpgm && in.name && std::string_view(in.name) == "s_endpgm") break;
  }
  return out;
}

}  // namespace xbem::gpu::rdna2
