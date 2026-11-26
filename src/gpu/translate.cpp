#include "gpu/translate.h"

#include <array>

#include <algorithm>
#include <cstring>
#include <format>
#include <map>
#include <set>
#include <string_view>

#include "gpu/guest_memory_layout.h"

namespace xbem::gpu {
namespace {

using rdna2::Format;

// ------------------------------------------------------------ operands

constexpr uint32_t kVccLo = 106, kVccHi = 107, kM0 = 124, kNull = 125, kExecLo = 126, kExecHi = 127,
                   kLiteral = 255, kScc = 253;
// Translator-internal operands: an SDWA source after its byte/word select.
constexpr uint32_t kSdwaSrc0 = 1000, kSdwaSrc1 = 1001;

bool is_sgpr_operand(uint32_t src) { return src <= kExecHi; }

// Inline constants (operand encodings 128-254), as 32-bit patterns.
bool inline_constant(uint32_t src, uint32_t& value) {
  if (src == 128) return value = 0, true;
  if (src >= 129 && src <= 192) return value = src - 128, true;
  if (src >= 193 && src <= 208) return value = static_cast<uint32_t>(-static_cast<int32_t>(src - 192)), true;
  switch (src) {
    case 240: value = 0x3f000000; return true;  // 0.5
    case 241: value = 0xbf000000; return true;
    case 242: value = 0x3f800000; return true;  // 1.0
    case 243: value = 0xbf800000; return true;
    case 244: value = 0x40000000; return true;  // 2.0
    case 245: value = 0xc0000000; return true;
    case 246: value = 0x40800000; return true;  // 4.0
    case 247: value = 0xc0800000; return true;
    case 248: value = 0x3e22f983; return true;  // 1 / (2 pi)
    default: return false;
  }
}

std::string_view name_of(const rdna2::Instruction& i) { return i.name ? i.name : ""; }
bool starts(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }

// Depth compare of four texels against a reference (S# compare function:
// never, less, equal, less-equal, greater, not-equal, greater-equal, always;
// reference OP texel), 1.0 where it passes.
constexpr const char* kCmpDepth =
    "vec4 cmp_depth(float r, vec4 t, uint f) {\n"
    "  bvec4 lt = lessThan(vec4(r), t), eq = equal(vec4(r), t), gt = greaterThan(vec4(r), t);\n"
    "  bvec4 p = bvec4(f == 7u);\n"
    "  if (f == 1u) p = lt; else if (f == 2u) p = eq; else if (f == 3u) p = not(gt);\n"
    "  else if (f == 4u) p = gt; else if (f == 5u) p = not(eq); else if (f == 6u) p = not(lt);\n"
    "  return vec4(p);\n"
    "}\n"
    // Cube map face selection (v_cubeid/ma/sc/tc): major axis z, then y, then x.
    "float cubeid(vec3 v) {\n"
    "  vec3 a = abs(v);\n"
    "  if (a.z >= a.x && a.z >= a.y) return v.z < 0.0 ? 5.0 : 4.0;\n"
    "  if (a.y >= a.x) return v.y < 0.0 ? 3.0 : 2.0;\n"
    "  return v.x < 0.0 ? 1.0 : 0.0;\n"
    "}\n"
    "float cubema(vec3 v) {\n"
    "  vec3 a = abs(v);\n"
    "  if (a.z >= a.x && a.z >= a.y) return 2.0 * v.z;\n"
    "  if (a.y >= a.x) return 2.0 * v.y;\n"
    "  return 2.0 * v.x;\n"
    "}\n"
    "float cubesc(vec3 v) {\n"
    "  vec3 a = abs(v);\n"
    "  if (a.z >= a.x && a.z >= a.y) return v.z < 0.0 ? -v.x : v.x;\n"
    "  if (a.y >= a.x) return v.x;\n"
    "  return v.x < 0.0 ? v.z : -v.z;\n"
    "}\n"
    "float cubetc(vec3 v) {\n"
    "  vec3 a = abs(v);\n"
    "  if (a.z >= a.x && a.z >= a.y) return -v.y;\n"
    "  if (a.y >= a.x) return v.y < 0.0 ? -v.z : v.z;\n"
    "  return -v.y;\n"
    "}\n";

bool is_scalar(Format f) {
  return f == Format::Sop1 || f == Format::Sop2 || f == Format::Sopk || f == Format::Sopc || f == Format::Sopp ||
         f == Format::Smem;
}

// ------------------------------------------------------------ fields

struct Vop {
  uint32_t dst = 0, src[3] = {}, abs = 0, neg = 0, clamp = 0, omod = 0, opsel = 0, opsel_hi = 0, sdst = 0;
  int count = 0;  // sources used by the encoding
  // SDWA (sub-dword addressing): sources and destination select a byte or
  // word (sel 0-3 BYTE0-3, 4-5 WORD0-1, 6 DWORD).
  bool sdwa = false;
  uint32_t sel[2] = {6, 6}, sext[2] = {}, dst_sel = 6, dst_unused = 0;
};

Vop vop_fields(const rdna2::Instruction& i) {
  Vop v;
  const uint32_t w0 = i.words[0], w1 = i.words[1];
  switch (i.format) {
    case Format::Vop2:
      v.src[0] = w0 & 0x1ff;
      v.src[1] = 256 + ((w0 >> 9) & 0xff);
      v.dst = (w0 >> 17) & 0xff;
      v.count = 2;
      break;
    case Format::Vop1:
      v.src[0] = w0 & 0x1ff;
      v.dst = (w0 >> 17) & 0xff;
      v.count = 1;
      break;
    case Format::Vopc:
      v.src[0] = w0 & 0x1ff;
      v.src[1] = 256 + ((w0 >> 9) & 0xff);
      v.dst = kVccLo;
      v.count = 2;
      break;
    case Format::Vop3:
      v.dst = w0 & 0xff;
      v.abs = (w0 >> 8) & 7;
      v.sdst = (w0 >> 8) & 0x7f;
      v.opsel = (w0 >> 11) & 0xf;
      v.clamp = (w0 >> 15) & 1;
      v.src[0] = w1 & 0x1ff;
      v.src[1] = (w1 >> 9) & 0x1ff;
      v.src[2] = (w1 >> 18) & 0x1ff;
      v.omod = (w1 >> 27) & 3;
      v.neg = (w1 >> 29) & 7;
      v.count = 3;
      break;
    case Format::Vop3p:
      v.dst = w0 & 0xff;
      v.opsel = (w0 >> 11) & 7;
      v.opsel_hi = ((w1 >> 27) & 3) | (((w0 >> 14) & 1) << 2);
      v.clamp = (w0 >> 15) & 1;
      v.src[0] = w1 & 0x1ff;
      v.src[1] = (w1 >> 9) & 0x1ff;
      v.src[2] = (w1 >> 18) & 0x1ff;
      v.neg = (w1 >> 29) & 7;
      v.count = 3;
      break;
    default: break;
  }
  if (i.extension == rdna2::Instruction::Extension::Sdwa &&
      (i.format == Format::Vop1 || i.format == Format::Vop2 || i.format == Format::Vopc)) {
    v.sdwa = true;
    v.src[0] = (w1 & 0xff) + (((w1 >> 23) & 1) ? 0 : 256);
    if (i.format != Format::Vop1 && ((w1 >> 31) & 1)) v.src[1] -= 256;  // src1 is an SGPR
    v.sel[0] = (w1 >> 16) & 7;
    v.sext[0] = (w1 >> 19) & 1;
    v.neg = ((w1 >> 20) & 1) | (((w1 >> 28) & 1) << 1);
    v.abs = ((w1 >> 21) & 1) | (((w1 >> 29) & 1) << 1);
    v.sel[1] = (w1 >> 24) & 7;
    v.sext[1] = (w1 >> 27) & 1;
    if (i.format == Format::Vopc) {
      v.sdst = ((w1 >> 15) & 1) ? (w1 >> 8) & 0x7f : kVccLo;
    } else {
      v.dst_sel = (w1 >> 8) & 7;
      v.dst_unused = (w1 >> 11) & 3;
      v.clamp = (w1 >> 13) & 1;
      v.omod = (w1 >> 14) & 3;
    }
  }
  return v;
}

// ------------------------------------------------------------ branches

// SOPP branches: target pc = pc + 4 + simm16 * 4.
bool is_branch(const rdna2::Instruction& i) {
  return i.format == Format::Sopp && i.name && std::string_view(i.name).starts_with("s_") &&
         (std::string_view(i.name) == "s_branch" || std::string_view(i.name).starts_with("s_cbranch_"));
}
uint32_t branch_target(const DecodedInstruction& d) {
  const int32_t simm = static_cast<int16_t>(d.insn.words[0] & 0xffff);
  return static_cast<uint32_t>(static_cast<int32_t>(d.pc) + 4 + simm * 4);
}

// ------------------------------------------------------------ translation

bool scalar_step(const rdna2::Instruction& insn, ScalarRun& run, bool& scc, bool& stop);

class Translator {
 public:
  Translator(TranslatedShader& out) : out_(out), wave_(out.stage == ShaderStage::Compute) {}

  // Basic blocks: split at branch targets and after branches. Only forward
  // branches (no loops); each block's code runs when the scalar run (which
  // follows the uniform branches on the CPU) reached it.
  bool split_blocks() {
    auto& insns = out_.program.instructions;
    std::set<uint32_t> leaders{0};
    for (uint32_t k = 0; k < insns.size(); ++k) {
      if (!is_branch(insns[k].insn)) continue;
      const uint32_t target = branch_target(insns[k]);
      if (target <= insns[k].pc) {
        index_ = k;
        return fail("loop (backward branch)");
      }
      leaders.insert(target);
      if (k + 1 < insns.size()) leaders.insert(insns[k + 1].pc);
    }
    out_.program.block_of.assign(insns.size(), 0);
    uint32_t block = 0;
    for (uint32_t k = 0; k < insns.size(); ++k) {
      if (k && leaders.count(insns[k].pc)) ++block;
      out_.program.block_of[k] = block;
    }
    blocks_ = block + 1;
    if (blocks_ > 32) return fail("more than 32 basic blocks");
    last_leader_pc_ = *leaders.rbegin();
    return true;
  }

  bool run() {
    body_.reserve(8192);
    if (wave_) return run_wave();
    if (!split_blocks()) return false;
    uint32_t mask_slot = 0, open_block = ~0u;
    if (blocks_ > 1) {
      index_ = 0;
      mask_slot = add_slot(SlotKind::Blocks, 0, 1);
    }
    for (uint32_t k = 0; k < out_.program.instructions.size(); ++k) {
      index_ = k;
      const rdna2::Instruction& insn = out_.program.instructions[k].insn;
      if (blocks_ > 1 && out_.program.block_of[k] != open_block) {
        if (open_block != ~0u) line("}");
        open_block = out_.program.block_of[k];
        line(std::format("if ((D({}) & {:#x}u) != 0u) {{", mask_slot, 1u << open_block));
      }
      if (is_scalar(insn.format)) {
        if (name_of(insn) == "s_endpgm") {
          if (out_.program.instructions[k].pc >= last_leader_pc_) break;
          continue;  // later blocks are reached by branches
        }
        if (!scalar_on_gpu(insn)) return false;
        continue;  // runs on the CPU (unless it reads an SGPR only the GPU knows)
      }
      if (name_of(insn) == "v_readfirstlane_b32") {
        // The value of the first active lane into an SGPR: the SGPR becomes a
        // GPU-side variable (each lane uses its own value - the title's uses
        // are uniform values that went through a VGPR).
        const uint32_t dst = (insn.words[0] >> 17) & 0xff;
        std::string a;
        if (dst >= 106 || !src(insn.words[0] & 0x1ff, insn, a)) return fail("v_readfirstlane_b32 operands");
        dyn_[dst] = true;
        line(std::format("sd{} = {};", dst, a));
        continue;
      }
      if (!emit(insn)) return false;
    }
    if (open_block != ~0u) line("}");
    finish();
    return true;
  }

  // ---------------------------------------------------------- wave mode
  //
  // Compute shaders: each shader invocation is one lane of a wave32 (the
  // Vulkan subgroup, 32 wide on the target device). Scalar instructions run
  // in every lane on the same values, so SGPRs, scc, vcc and exec are
  // ordinary variables that agree across the wave; vector instructions are
  // guarded by the lane's exec bit; lane-crossing operations (compares into
  // masks, readlane) use subgroup operations outside the guards, where all
  // invocations are converged. Control flow is a block-dispatch loop, which
  // is uniform per wave - loops and divergent branches (exec masking) work.
  // Only memory the GPU cannot reach comes from the CPU: user SGPRs and
  // SMEM results (constants, descriptors) are per-dispatch slots, which needs
  // their addresses to be known on the CPU (see known_sgprs()).
  bool run_wave() {
    auto& insns = out_.program.instructions;
    if (!known_sgprs()) return false;
    std::map<uint32_t, uint32_t> block_at;  // pc -> block
    std::set<uint32_t> leaders{0};
    for (uint32_t k = 0; k < insns.size(); ++k) {
      const std::string_view n = name_of(insns[k].insn);
      if (is_branch(insns[k].insn)) leaders.insert(branch_target(insns[k]));
      if ((is_branch(insns[k].insn) || n == "s_endpgm") && k + 1 < insns.size()) leaders.insert(insns[k + 1].pc);
    }
    uint32_t next = 0;
    for (uint32_t pc : leaders) block_at[pc] = next++;
    const auto block_of = [&](uint32_t pc, uint32_t& b) {
      const auto it = block_at.find(pc);
      if (it == block_at.end()) return false;
      b = it->second;
      return true;
    };
    // User SGPRs come from the dispatch.
    for (uint32_t k = 0; k < out_.compute.user_sgprs && k < 32; ++k) {
      index_ = 0;
      line(std::format("{} = D({});", sreg(k), add_slot(SlotKind::Sgpr, k, 1)));
    }
    uint32_t r = out_.compute.user_sgprs;
    for (int k = 0; k < 3; ++k)
      if (out_.compute.tgid[k]) line(std::format("{} = gl_WorkGroupID.{};", sreg(r++), "xyz"[k]));
    if (out_.compute.tg_size) line(std::format("{} = gl_NumSubgroups | (gl_SubgroupID << 6);", sreg(r++)));
    line("uint blk = 0u;");
    line("while (blk != 0xffffffffu) {");
    line("switch (blk) {");
    bool open = false;
    for (uint32_t k = 0; k < insns.size(); ++k) {
      index_ = k;
      const rdna2::Instruction& insn = insns[k].insn;
      const std::string_view n = name_of(insn);
      if (block_at.count(insns[k].pc)) {
        if (open) line(std::format("blk = {}u; break; }}", block_at[insns[k].pc]));
        line(std::format("case {}u: {{", block_at[insns[k].pc]));
        open = true;
      }
      if (!open) continue;  // unreachable code after an unconditional transfer
      if (is_branch(insn)) {
        uint32_t target;
        if (!block_of(branch_target(insns[k]), target)) return fail("branch target outside the program");
        std::string cond;
        if (n == "s_branch") cond = "true";
        else if (n == "s_cbranch_scc0") cond = "!scc";
        else if (n == "s_cbranch_scc1") cond = "scc";
        else if (n == "s_cbranch_vccz") cond = "vcc_lo == 0u";
        else if (n == "s_cbranch_vccnz") cond = "vcc_lo != 0u";
        else if (n == "s_cbranch_execz") cond = "exec_lo == 0u";
        else if (n == "s_cbranch_execnz") cond = "exec_lo != 0u";
        else return fail("");
        if (cond == "true") {
          line(std::format("blk = {}u; break; }}", target));
          open = false;
        } else {
          line(std::format("if ({}) {{ blk = {}u; break; }}", cond, target));
        }
        continue;
      }
      if (n == "s_endpgm") {
        line("blk = 0xffffffffu; break; }");
        open = false;
        continue;
      }
      if (is_scalar(insn.format)) {
        if (!emit_scalar_wave(insn, k)) return false;
        continue;
      }
      if (!emit_vector_wave(insn)) return false;
    }
    if (open) line("blk = 0xffffffffu; break; }");
    line("default: blk = 0xffffffffu; break;");
    line("}");
    line("}");
    finish_wave();
    return true;
  }

  // Vector instructions: lane-crossing ones first, then the guarded rest.
  bool emit_vector_wave(const rdna2::Instruction& insn) {
    const std::string_view n = name_of(insn);
    const Vop v = vop_fields(insn);
    std::string a, b;
    if (n == "v_readlane_b32") {
      if (!src(v.src[0], insn, a) || !src(v.src[1], insn, b)) return false;
      line(std::format("{} = subgroupShuffle({}, {} & 31u);", sreg(v.dst), a, b));
      return true;
    }
    if (n == "v_readfirstlane_b32") {
      const uint32_t dst = (insn.words[0] >> 17) & 0xff;
      if (!src(insn.words[0] & 0x1ff, insn, a)) return false;
      line(std::format("{} = subgroupShuffle({}, exec_lo == 0u ? 0u : uint(findLSB(exec_lo)));", sreg(dst), a));
      return true;
    }
    if (n == "v_writelane_b32") {
      if (!src(v.src[0], insn, a) || !src(v.src[1], insn, b)) return false;
      line(std::format("if (lane == ({} & 31u)) {} = {};", b, vreg(v.dst), a));
      return true;
    }
    if (n == "ds_bpermute_b32" || n == "ds_permute_b32") {
      const uint32_t w1 = insn.words[1];
      const uint32_t addr = w1 & 0xff, data0 = (w1 >> 8) & 0xff, vdst = (w1 >> 24) & 0xff;
      const uint32_t offset = insn.words[0] & 0xffff;
      if (n == "ds_permute_b32") return fail("ds_permute_b32");
      // bpermute: each active lane reads lane (addr + offset) / 4 (inactive sources read as 0).
      line(std::format("{{ uint src_lane = (({} + {}u) >> 2) & 31u; uint val = subgroupShuffle({}, src_lane);"
                       " bool src_on = ((exec_lo >> src_lane) & 1u) != 0u;",
                       vreg(addr), offset, vreg(data0)));
      line(std::format("  if (lane_on()) {} = src_on ? val : 0u; }}", vreg(vdst)));
      return true;
    }
    if (n == "v_permlane16_b32" || n == "v_permlanex16_b32") {
      // Lane i of each row of 16 reads lane sel[i] of its row (x16: of the
      // other row); sel[0..7] are nibbles of src1, sel[8..15] of src2.
      // op_sel: bit 0 fetch inactive lanes too, bit 1 bound control.
      std::string s0, s1, s2;
      if (!src(v.src[0], insn, s0) || !src(v.src[1], insn, s1) || !src(v.src[2], insn, s2)) return false;
      const bool fi = v.opsel & 1, bc = (v.opsel >> 1) & 1;
      line(std::format("{{ uint i = lane & 15u; uint sel = i < 8u ? ({} >> (i * 4u)) & 15u : ({} >> ((i - 8u) * 4u)) & 15u;", s1, s2));
      line(std::format("  uint from = ((lane & ~15u) ^ {}u) | sel; uint val = subgroupShuffle({}, from & 31u);",
                       n == "v_permlanex16_b32" ? 16 : 0, s0));
      line(std::format("  bool on = {} || ((exec_lo >> (from & 31u)) & 1u) != 0u;", fi ? "true" : "false"));
      line(std::format("  if (lane_on()) {} = on ? val : {}; }}", vreg(v.dst), bc ? "0u" : vreg(v.dst)));
      return true;
    }
    if (starts(n, "v_cmp") || n.find("_co_") != std::string_view::npos) line("cmpv = false;");
    // DPP16: the source operand comes from another lane of the row (a
    // shuffle, so outside the lane guard).
    if (insn.extension == rdna2::Instruction::Extension::Dpp16) {
      const uint32_t w1 = insn.words[1];
      const uint32_t src0 = w1 & 0xff, ctrl = (w1 >> 8) & 0x1ff, bound_ctrl = (w1 >> 19) & 1;
      std::string from;
      if (ctrl <= 0xff) from = std::format("(lane & ~3u) | ((0x{:x}u >> ((lane & 3u) * 2u)) & 3u)", ctrl);
      else if (ctrl >= 0x101 && ctrl <= 0x10f) from = std::format("lane + {}u", ctrl & 15);          // row_shl
      else if (ctrl >= 0x111 && ctrl <= 0x11f) from = std::format("lane - {}u", ctrl & 15);          // row_shr
      else if (ctrl >= 0x121 && ctrl <= 0x12f) from = std::format("(lane & ~15u) | ((lane - {}u) & 15u)", ctrl & 15);  // row_ror
      else if (ctrl == 0x140) from = "(lane & ~15u) | (15u - (lane & 15u))";                          // row_mirror
      else if (ctrl == 0x141) from = "(lane & ~7u) | (7u - (lane & 7u))";                             // row_half_mirror
      else if (ctrl >= 0x150 && ctrl <= 0x15f) from = std::format("(lane & ~15u) | {}u", ctrl & 15);  // row_share
      else if (ctrl >= 0x160 && ctrl <= 0x16f) from = std::format("lane ^ {}u", ctrl & 15);          // row_xmask
      else return fail(std::format("DPP control {:#x}", ctrl));
      const bool shift = (ctrl >= 0x101 && ctrl <= 0x11f);
      line(std::format("{{ uint from = {}; dppv = subgroupShuffle({}, from & 31u);", from, vreg(src0)));
      line(std::format("  bool valid = {} && ((exec_lo >> (from & 31u)) & 1u) != 0u; if (!valid) dppv = {}; }}",
                       shift ? "((from & ~15u) == (lane & ~15u)) && from < 32u" : "true",
                       bound_ctrl ? "0u" : vreg(v.dst)));
      dpp_ = true;
    }
    line("if (lane_on()) {");
    post_.clear();
    bool ok;
    switch (insn.format) {
      case Format::Ds: ok = emit_ds(insn); break;
      case Format::Mubuf: ok = emit_mubuf(insn); break;
      case Format::Mtbuf: ok = emit_mtbuf(insn); break;
      case Format::Mimg: ok = emit_mimg(insn); break;
      case Format::Global: ok = emit_global(insn); break;
      default: ok = emit(insn); break;
    }
    if (!ok) return false;
    line("}");
    for (const std::string& p : post_) line(p);
    post_.clear();
    return true;
  }

  // A 64-bit scalar operand as a uint64_t expression (inline constants sign-extend).
  bool src64(uint32_t s, const rdna2::Instruction& insn, std::string& e) {
    uint32_t c;
    if (s == kLiteral) return e = std::format("uint64_t({:#x}u)", insn.literal), true;
    if (inline_constant(s, c)) {
      if (s >= 240) return fail("64-bit float inline constant");
      return e = std::format("uint64_t(int64_t(int({:#x}u)))", c), true;
    }
    if (s == kScc) return e = "uint64_t(scc)", true;
    if (s < 106 || s == kVccLo || s == kExecLo) {
      e = std::format("packUint2x32(uvec2({}, {}))", sreg(s), sreg(s + 1));
      return true;
    }
    if (s == kNull) return e = "uint64_t(0)", true;
    return fail(std::format("64-bit operand {}", s));
  }
  void set64(uint32_t d, const std::string& value) {
    line(std::format("{{ uvec2 t64 = unpackUint2x32({}); {} = t64.x; {} = t64.y; }}", value, sreg(d), sreg(d + 1)));
  }

  bool emit_scalar_wave(const rdna2::Instruction& insn, uint32_t k) {
    const std::string_view n = name_of(insn);
    const uint32_t w0 = insn.words[0], w1 = insn.words[1];
    std::string a, b;
    switch (insn.format) {
      case Format::Sopp: {
        if (n == "s_barrier") line("barrier();");
        // waits, hints, messages, priorities: nothing to do
        return true;
      }
      case Format::Smem: {
        const uint32_t sdata = (w0 >> 6) & 0x7f;
        if (n == "s_dcache_inv" || n == "s_gl1_inv" || n == "s_dcache_wb") return true;
        if (n == "s_memtime" || n == "s_memrealtime") {
          line(std::format("{} = 0u; {} = 0u;", sreg(sdata), sreg(sdata + 1)));
          return true;
        }
        int words = 1;
        if (n.ends_with("x2")) words = 2;
        else if (n.ends_with("x4")) words = 4;
        else if (n.ends_with("x8")) words = 8;
        else if (n.ends_with("x16")) words = 16;
        if (!starts(n, "s_load_dword") && !starts(n, "s_buffer_load_dword")) return fail("");
        if (gpu_load_.count(k)) {
          const uint32_t sbase = (w0 & 0x3f) * 2, soffset = (w1 >> 25) & 0x7f;
          const uint32_t imm = w1 & 0x1fffff;
          std::string so;
          if (!src(soffset, insn, so)) return false;
          const bool buffer = starts(n, "s_buffer");
          // s_load: a 64-bit address; s_buffer_load: a V# (48-bit base, records in dword 2).
          line(std::format("{{ uint off = ({} + {:#x}u) & ~3u; uint64_t a = packUint2x32(uvec2({}, {})) + uint64_t(off);", so,
                           imm, sreg(sbase), buffer ? std::format("{} & 0xffffu", sreg(sbase + 1)) : sreg(sbase + 1)));
          for (int i = 0; i < words; ++i)
            line(buffer ? std::format("  {} = off + {}u < {} ? ld32(a + {}ul) : 0u;", sreg(sdata + i), i * 4 + 3, sreg(sbase + 2), i * 4)
                        : std::format("  {} = ld32(a + {}ul);", sreg(sdata + i), i * 4));
          line("}");
          return true;
        }
        if (gpu_smem_.count(k)) {
          const uint32_t sbase = (w0 & 0x3f) * 2, soffset = (w1 >> 25) & 0x7f;
          const uint32_t imm = w1 & 0x1fffff;
          const uint32_t buf = add_slot(SlotKind::Buffer, sbase, 5);
          std::string so;
          if (!src(soffset, insn, so)) return false;
          line(std::format("{{ uint off = ({} + {:#x}u) & ~3u; uint64_t a = packUint2x32(uvec2(D({}), D({} + 1u))) + uint64_t(off);",
                           so, imm, buf, buf));
          for (int i = 0; i < words; ++i)
            line(std::format("  {} = off + {}u < D({} + 3u) ? ld32(a + {}ul) : 0u;", sreg(sdata + i), i * 4 + 3, buf, i * 4));
          line("}");
          return true;
        }
        // The CPU performs the load (its address is known there): the words
        // come in as slots captured right after this instruction.
        index_ = k + 1;
        for (int i = 0; i < words; ++i)
          line(std::format("{} = D({});", sreg(sdata + i), add_slot(SlotKind::Sgpr, sdata + i, 1)));
        index_ = k;
        return true;
      }
      case Format::Sopk: {
        const uint32_t dst = (w0 >> 16) & 0x7f;
        const int32_t imm = static_cast<int16_t>(w0 & 0xffff);
        const std::string d = sreg(dst);
        const std::string si = std::format("int({})", imm), ui = std::format("{:#x}u", static_cast<uint32_t>(w0 & 0xffff));
        if (n == "s_movk_i32") line(std::format("{} = uint({});", d, si));
        else if (n == "s_cmovk_i32") line(std::format("if (scc) {} = uint({});", d, si));
        else if (n == "s_addk_i32")
          line(std::format("{{ int o = int({0}); int r = o + {1}; scc = ((o >= 0) == ({1} >= 0)) && ((r >= 0) != (o >= 0)); {0} = uint(r); }}", d, si));
        else if (n == "s_mulk_i32") line(std::format("{0} = uint(int({0}) * {1});", d, si));
        else if (starts(n, "s_cmpk_")) {
          static const std::pair<std::string_view, const char*> kOps[] = {
              {"eq", "=="}, {"lg", "!="}, {"gt", ">"}, {"ge", ">="}, {"lt", "<"}, {"le", "<="}};
          const std::string_view rest = n.substr(7);
          const std::string_view op = rest.substr(0, 2), type = rest.substr(3);
          const char* c = nullptr;
          for (const auto& [kk, o] : kOps)
            if (op == kk) c = o;
          if (!c) return fail("");
          if (type == "i32") line(std::format("scc = int({}) {} {};", d, c, si));
          else line(std::format("scc = {} {} {};", d, c, ui));
        } else if (n == "s_getreg_b32") line(std::format("{} = 0u;", d));
        else if (n == "s_setreg_b32" || n == "s_setreg_imm32_b32" || n == "s_waitcnt_vscnt" || n == "s_waitcnt_vmcnt" ||
                 n == "s_waitcnt_expcnt" || n == "s_waitcnt_lgkmcnt" || n == "s_version")
          ;
        else return fail("");
        return true;
      }
      case Format::Sopc: {
        const uint32_t s0 = w0 & 0xff, s1 = (w0 >> 8) & 0xff;
        if (n == "s_cmp_eq_u64" || n == "s_cmp_lg_u64") {
          if (!src64(s0, insn, a) || !src64(s1, insn, b)) return false;
          line(std::format("scc = {} {} {};", a, n == "s_cmp_eq_u64" ? "==" : "!=", b));
          return true;
        }
        if (n == "s_bitcmp0_b64" || n == "s_bitcmp1_b64") {
          if (!src64(s0, insn, a) || !src(s1, insn, b)) return false;
          line(std::format("scc = (({} >> ({} & 63u)) & uint64_t(1)) {} uint64_t(0);", a, b, n == "s_bitcmp1_b64" ? "!=" : "=="));
          return true;
        }
        if (!src(s0, insn, a) || !src(s1, insn, b)) return false;
        static const std::pair<std::string_view, const char*> kOps[] = {
            {"eq", "=="}, {"lg", "!="}, {"gt", ">"}, {"ge", ">="}, {"lt", "<"}, {"le", "<="}};
        if (n == "s_bitcmp0_b32" || n == "s_bitcmp1_b32") {
          line(std::format("scc = (({} >> ({} & 31u)) & 1u) {} 0u;", a, b, n == "s_bitcmp1_b32" ? "!=" : "=="));
          return true;
        }
        if (!starts(n, "s_cmp_")) return fail("");
        const std::string_view op = n.substr(6, 2), type = n.substr(9);
        const char* c = nullptr;
        for (const auto& [kk, o] : kOps)
          if (op == kk) c = o;
        if (!c) return fail("");
        if (type == "i32") line(std::format("scc = int({}) {} int({});", a, c, b));
        else line(std::format("scc = {} {} {};", a, c, b));
        return true;
      }
      case Format::Sop1: return emit_sop1_wave(insn);
      case Format::Sop2: return emit_sop2_wave(insn);
      default: return fail("");
    }
  }

  bool emit_sop1_wave(const rdna2::Instruction& insn) {
    const std::string_view n = name_of(insn);
    const uint32_t w0 = insn.words[0], dst = (w0 >> 16) & 0x7f, s0 = w0 & 0xff;
    const std::string d = sreg(dst);
    std::string a;
    // exec manipulation: (and|or|xor|andn1|andn2|orn1|orn2|nand|nor|xnor)_saveexec, (andn1|andn2)_wrexec
    if (n.find("_saveexec_") != std::string_view::npos || n.find("_wrexec_") != std::string_view::npos) {
      if (!src(s0, insn, a)) return false;
      const std::string_view op = n.substr(2, n.find('_', 2) - 2);
      std::string e;
      if (op == "and") e = std::format("{} & exec_lo", a);
      else if (op == "or") e = std::format("{} | exec_lo", a);
      else if (op == "xor") e = std::format("{} ^ exec_lo", a);
      else if (op == "andn1") e = std::format("~{} & exec_lo", a);
      else if (op == "andn2") e = std::format("{} & ~exec_lo", a);
      else if (op == "orn1") e = std::format("~{} | exec_lo", a);
      else if (op == "orn2") e = std::format("{} | ~exec_lo", a);
      else if (op == "nand") e = std::format("~({} & exec_lo)", a);
      else if (op == "nor") e = std::format("~({} | exec_lo)", a);
      else if (op == "xnor") e = std::format("~({} ^ exec_lo)", a);
      else return fail("");
      const bool wide = n.ends_with("_b64");
      if (n.find("_wrexec_") != std::string_view::npos) {
        line(std::format("exec_lo = ({}) & wave_lanes; scc = exec_lo != 0u; {} = exec_lo;", e, d));
      } else {
        line(std::format("{{ uint old = exec_lo; exec_lo = ({}) & wave_lanes; scc = exec_lo != 0u; {} = old; }}", e, d));
      }
      if (wide) line(std::format("{} = 0u;", sreg(dst + 1)));
      return true;
    }
    if (n == "s_bitreplicate_b64_b32") {
      // Every bit of the source twice (bit i -> bits 2i and 2i + 1).
      if (!src(s0, insn, a)) return false;
      line(std::format("{{ uint x = {}, lo = 0u, hi = 0u;", a));
      line("  for (uint i = 0u; i < 16u; ++i) { lo |= ((x >> i) & 1u) * (3u << (2u * i)); hi |= ((x >> (i + 16u)) & 1u) * (3u << (2u * i)); }");
      set64(dst, "packUint2x32(uvec2(lo, hi))");
      line("}");
      return true;
    }
    if (n.ends_with("_b64") && n != "s_bcnt1_i32_b64" && n != "s_bcnt0_i32_b64" && n != "s_ff1_i32_b64" &&
        n != "s_ff0_i32_b64" && n != "s_flbit_i32_b64") {
      if (!src64(s0, insn, a)) return false;
      if (n == "s_mov_b64") return set64(dst, a), true;
      if (n == "s_cmov_b64") return line("if (scc) {"), set64(dst, a), line("}"), true;
      if (n == "s_not_b64") return line(std::format("{{ uint64_t r = ~{}; scc = r != uint64_t(0);", a)), set64(dst, "r"), line("}"), true;
      if (n == "s_wqm_b64") {
        line(std::format("{{ uvec2 m = unpackUint2x32({}); uvec2 q = m | (m >> 1u) | (m >> 2u) | (m >> 3u); q &= uvec2(0x11111111u); q *= 15u;", a));
        line("  scc = any(notEqual(q, uvec2(0u)));");
        set64(dst, "packUint2x32(q)");
        line("}");
        return true;
      }
      if (n == "s_brev_b64") {
        line(std::format("{{ uvec2 m = unpackUint2x32({});", a));
        set64(dst, "packUint2x32(uvec2(bitfieldReverse(m.y), bitfieldReverse(m.x)))");
        line("}");
        return true;
      }
      return fail("");
    }
    if (!src(s0, insn, a)) return false;
    if (n == "s_mov_b32") line(std::format("{} = {};", d, a));
    else if (n == "s_cmov_b32") line(std::format("if (scc) {} = {};", d, a));
    else if (n == "s_not_b32") line(std::format("{0} = ~{1}; scc = {0} != 0u;", d, a));
    else if (n == "s_wqm_b32")
      line(std::format("{{ uint m = {1}; uint q = (m | (m >> 1u) | (m >> 2u) | (m >> 3u)) & 0x11111111u; {0} = q * 15u; scc = {0} != 0u; }}", d, a));
    else if (n == "s_brev_b32") line(std::format("{} = bitfieldReverse({});", d, a));
    else if (n == "s_bcnt0_i32_b32") line(std::format("{0} = uint(32 - bitCount({1})); scc = {0} != 0u;", d, a));
    else if (n == "s_bcnt1_i32_b32") line(std::format("{0} = uint(bitCount({1})); scc = {0} != 0u;", d, a));
    else if (n == "s_bcnt1_i32_b64") {
      std::string a64;
      if (!src64(s0, insn, a64)) return false;
      line(std::format("{{ uvec2 m = unpackUint2x32({1}); {0} = uint(bitCount(m.x) + bitCount(m.y)); scc = {0} != 0u; }}", d, a64));
    } else if (n == "s_ff1_i32_b32") line(std::format("{} = uint(findLSB({}));", d, a));
    else if (n == "s_ff0_i32_b32") line(std::format("{} = uint(findLSB(~{}));", d, a));
    else if (n == "s_ff1_i32_b64") {
      std::string a64;
      if (!src64(s0, insn, a64)) return false;
      line(std::format("{{ uvec2 m = unpackUint2x32({1}); {0} = m.x != 0u ? uint(findLSB(m.x)) : m.y != 0u ? uint(findLSB(m.y)) + 32u : 0xffffffffu; }}", d, a64));
    } else if (n == "s_flbit_i32_b32") line(std::format("{{ uint m = {1}; {0} = m == 0u ? 0xffffffffu : uint(31 - findMSB(m)); }}", d, a));
    else if (n == "s_flbit_i32") line(std::format("{{ int m = int({1}); {0} = (m == 0 || m == -1) ? 0xffffffffu : uint(31 - findMSB(m)); }}", d, a));
    else if (n == "s_sext_i32_i8") line(std::format("{} = uint(bitfieldExtract(int({}), 0, 8));", d, a));
    else if (n == "s_sext_i32_i16") line(std::format("{} = uint(bitfieldExtract(int({}), 0, 16));", d, a));
    else if (n == "s_bitset0_b32") line(std::format("{0} &= ~(1u << ({1} & 31u));", d, a));
    else if (n == "s_bitset1_b32") line(std::format("{0} |= 1u << ({1} & 31u);", d, a));
    else if (n == "s_abs_i32") line(std::format("{0} = uint(abs(int({1}))); scc = {0} != 0u;", d, a));
    else if (n == "s_quadmask_b32")
      line(std::format("{{ uint m = {1}, r = 0u; for (int i = 0; i < 8; ++i) if (((m >> (i * 4)) & 15u) != 0u) r |= 1u << i; {0} = r; scc = r != 0u; }}", d, a));
    else return fail("");
    return true;
  }

  bool emit_sop2_wave(const rdna2::Instruction& insn) {
    const std::string_view n = name_of(insn);
    const uint32_t w0 = insn.words[0], dst = (w0 >> 16) & 0x7f, s0 = w0 & 0xff, s1 = (w0 >> 8) & 0xff;
    std::string a, b;
    const std::string d = sreg(dst);
    if (n.ends_with("_b64") || n.ends_with("_u64") || n.ends_with("_i64")) {
      if (n == "s_lshl_b64" || n == "s_lshr_b64" || n == "s_ashr_i64" || n == "s_bfe_u64" || n == "s_bfe_i64") {
        if (!src64(s0, insn, a) || !src(s1, insn, b)) return false;
        std::string e;
        if (n == "s_lshl_b64") e = std::format("{} << ({} & 63u)", a, b);
        else if (n == "s_lshr_b64") e = std::format("{} >> ({} & 63u)", a, b);
        else if (n == "s_ashr_i64") e = std::format("uint64_t(int64_t({}) >> ({} & 63u))", a, b);
        else return fail("");
        line(std::format("{{ uint64_t r = {}; scc = r != uint64_t(0);", e));
        set64(dst, "r");
        line("}");
        return true;
      }
      if (n == "s_bfm_b64") {
        if (!src(s0, insn, a) || !src(s1, insn, b)) return false;
        line(std::format("{{ uint w = {} & 63u; uint64_t r = (w == 0u ? uint64_t(0) : (uint64_t(0xffffffffffffffffUL) >> (64u - w))) << ({} & 63u);", a, b));
        set64(dst, "r");
        line("}");
        return true;
      }
      if (!src64(s0, insn, a) || !src64(s1, insn, b)) return false;
      std::string e;
      bool sets_scc = true;
      if (n == "s_and_b64") e = std::format("{} & {}", a, b);
      else if (n == "s_or_b64") e = std::format("{} | {}", a, b);
      else if (n == "s_xor_b64") e = std::format("{} ^ {}", a, b);
      else if (n == "s_andn2_b64") e = std::format("{} & ~{}", a, b);
      else if (n == "s_orn2_b64") e = std::format("{} | ~{}", a, b);
      else if (n == "s_nand_b64") e = std::format("~({} & {})", a, b);
      else if (n == "s_nor_b64") e = std::format("~({} | {})", a, b);
      else if (n == "s_xnor_b64") e = std::format("~({} ^ {})", a, b);
      else if (n == "s_cselect_b64") e = std::format("scc ? {} : {}", a, b), sets_scc = false;
      else return fail("");
      line(std::format("{{ uint64_t r = {};{}", e, sets_scc ? " scc = r != uint64_t(0);" : ""));
      set64(dst, "r");
      line("}");
      return true;
    }
    if (!src(s0, insn, a) || !src(s1, insn, b)) return false;
    // Results go through a temporary: the destination may be a source.
    std::string e, scc;
    if (n == "s_add_u32") e = std::format("{} + {}", a, b), scc = std::format("r < {}", a);
    else if (n == "s_addc_u32") e = std::format("{} + {} + uint(scc)", a, b), scc = std::format("(uint64_t({}) + uint64_t({}) + uint64_t(scc)) > uint64_t(0xffffffffu)", a, b);
    else if (n == "s_sub_u32") e = std::format("{} - {}", a, b), scc = std::format("{} > {}", b, a);
    else if (n == "s_subb_u32") e = std::format("{} - {} - uint(scc)", a, b), scc = std::format("uint64_t({}) + uint64_t(scc) > uint64_t({})", b, a);
    else if (n == "s_add_i32") e = std::format("uint(int({}) + int({}))", a, b), scc = std::format("((int({0}) >= 0) == (int({1}) >= 0)) && ((int(r) >= 0) != (int({0}) >= 0))", a, b);
    else if (n == "s_sub_i32") e = std::format("uint(int({}) - int({}))", a, b), scc = std::format("((int({0}) >= 0) != (int({1}) >= 0)) && ((int(r) >= 0) != (int({0}) >= 0))", a, b);
    else if (n == "s_mul_i32") e = std::format("uint(int({}) * int({}))", a, b);
    else if (n == "s_mul_hi_u32") e = std::format("uint((uint64_t({}) * uint64_t({})) >> 32)", a, b);
    else if (n == "s_mul_hi_i32") e = std::format("uint((int64_t(int({})) * int64_t(int({}))) >> 32)", a, b);
    else if (n == "s_and_b32") e = std::format("{} & {}", a, b), scc = "r != 0u";
    else if (n == "s_or_b32") e = std::format("{} | {}", a, b), scc = "r != 0u";
    else if (n == "s_xor_b32") e = std::format("{} ^ {}", a, b), scc = "r != 0u";
    else if (n == "s_andn2_b32") e = std::format("{} & ~{}", a, b), scc = "r != 0u";
    else if (n == "s_orn2_b32") e = std::format("{} | ~{}", a, b), scc = "r != 0u";
    else if (n == "s_nand_b32") e = std::format("~({} & {})", a, b), scc = "r != 0u";
    else if (n == "s_nor_b32") e = std::format("~({} | {})", a, b), scc = "r != 0u";
    else if (n == "s_xnor_b32") e = std::format("~({} ^ {})", a, b), scc = "r != 0u";
    else if (n == "s_lshl_b32") e = std::format("{} << ({} & 31u)", a, b), scc = "r != 0u";
    else if (n == "s_lshr_b32") e = std::format("{} >> ({} & 31u)", a, b), scc = "r != 0u";
    else if (n == "s_ashr_i32") e = std::format("uint(int({}) >> int({} & 31u))", a, b), scc = "r != 0u";
    else if (n == "s_min_u32") e = std::format("min({}, {})", a, b), scc = std::format("{} < {}", a, b);
    else if (n == "s_max_u32") e = std::format("max({}, {})", a, b), scc = std::format("{} > {}", a, b);
    else if (n == "s_min_i32") e = std::format("uint(min(int({}), int({})))", a, b), scc = std::format("int({}) < int({})", a, b);
    else if (n == "s_max_i32") e = std::format("uint(max(int({}), int({})))", a, b), scc = std::format("int({}) > int({})", a, b);
    else if (n == "s_cselect_b32") e = std::format("scc ? {} : {}", a, b);
    else if (n == "s_bfm_b32") e = std::format("bitfieldInsert(0u, 0xffffffffu, int({1} & 31u), int({0} & 31u))", a, b);
    else if (n == "s_bfe_u32")
      e = std::format("(({1} >> 16) & 0x7fu) == 0u ? 0u : bitfieldExtract({0}, int({1} & 31u), int(min((({1} >> 16) & 0x7fu), 32u - ({1} & 31u))))", a, b), scc = "r != 0u";
    else if (n == "s_bfe_i32")
      e = std::format("(({1} >> 16) & 0x7fu) == 0u ? 0u : uint(bitfieldExtract(int({0}), int({1} & 31u), int(min((({1} >> 16) & 0x7fu), 32u - ({1} & 31u)))))", a, b), scc = "r != 0u";
    else if (n == "s_lshl1_add_u32" || n == "s_lshl2_add_u32" || n == "s_lshl3_add_u32" || n == "s_lshl4_add_u32") {
      const int sh = n[6] - '0';
      e = std::format("({} << {}u) + {}", a, sh, b);
      scc = std::format("(uint64_t({}) << {}) + uint64_t({}) > uint64_t(0xffffffffu)", a, sh, b);
    } else if (n == "s_pack_ll_b32_b16") e = std::format("({} & 0xffffu) | ({} << 16)", a, b);
    else if (n == "s_pack_lh_b32_b16") e = std::format("({} & 0xffffu) | ({} & 0xffff0000u)", a, b);
    else if (n == "s_pack_hh_b32_b16") e = std::format("({} >> 16) | ({} & 0xffff0000u)", a, b);
    else if (n == "s_absdiff_i32") e = std::format("uint(abs(int({}) - int({})))", a, b), scc = "r != 0u";
    else return fail("");
    line(std::format("{{ uint r = {};{} {} = r; }}", e, scc.empty() ? "" : std::format(" scc = {};", scc), d));
    return true;
  }

  // Which SGPRs the CPU can know (in program order, ignoring branches):
  // user SGPRs, constants, SMEM results and scalar arithmetic on known
  // values - nothing derived from lane data (compares, readlane), the
  // workgroup id or exec, and at a loop's head nothing the loop writes
  // (values carried round the loop; values the body computes from known
  // inputs are the same every iteration). SMEM addresses and
  // the descriptors of vector memory instructions must be known: the CPU
  // performs those loads and turns descriptors into bindless indices.
  bool known_sgprs() {
    auto& insns = out_.program.instructions;
    std::array<bool, 128> known{};
    std::map<uint32_t, std::array<bool, 128>> loop_writes;  // loop head pc -> SGPRs the loop writes
    const auto scalar_dsts = [](const rdna2::Instruction& i, uint32_t& d, uint32_t& count) {
      const std::string_view n = name_of(i);
      const uint32_t w0 = i.words[0];
      count = 0;
      switch (i.format) {
        case Format::Sop1: case Format::Sop2:
          d = (w0 >> 16) & 0x7f;
          count = (n.ends_with("_b64") || n.ends_with("_u64") || n.ends_with("_i64")) &&
                          !starts(n, "s_bcnt") && !starts(n, "s_ff") && !starts(n, "s_flbit") && !starts(n, "s_bitcmp")
                      ? 2 : 1;
          break;
        case Format::Sopk:
          d = (w0 >> 16) & 0x7f;
          count = starts(n, "s_cmpk_") ? 0 : 1;
          break;
        case Format::Smem: {
          d = (w0 >> 6) & 0x7f;
          count = n.ends_with("x16") ? 16 : n.ends_with("x8") ? 8 : n.ends_with("x4") ? 4 : n.ends_with("x2") ? 2 : 1;
          if (!starts(n, "s_load") && !starts(n, "s_buffer_load")) count = starts(n, "s_mem") ? 2 : 0;
          break;
        }
        case Format::Vop3: case Format::Vop1:
          if (n == "v_readlane_b32") d = i.words[0] & 0xff, count = 1;
          else if (n == "v_readfirstlane_b32") d = (w0 >> 17) & 0xff, count = 1;
          else if (i.format == Format::Vop3 && starts(n, "v_cmp")) d = (w0 >> 8) & 0x7f, count = 2;
          break;
        case Format::Vopc:  // SDWA: an SGPR destination when SD is set
          if ((w0 & 0x1ff) == 0xf9 && ((i.words[1] >> 15) & 1)) d = (i.words[1] >> 8) & 0x7f, count = 1;
          break;
        default: break;
      }
    };
    for (uint32_t k = 0; k < insns.size(); ++k) {
      if (!is_branch(insns[k].insn)) continue;
      const uint32_t target = branch_target(insns[k]);
      if (target > insns[k].pc) continue;
      auto& w = loop_writes[target];
      for (uint32_t j = 0; j <= k; ++j) {
        if (insns[j].pc < target) continue;
        uint32_t d, c;
        scalar_dsts(insns[j].insn, d, c);
        for (uint32_t x = d; x < d + c && x < 128; ++x) w[x] = true;
      }
    }
    std::array<bool, 128> loopy{};  // (nothing is unknown merely for being written in a loop)
    for (uint32_t k = 0; k < out_.compute.user_sgprs && k < 128; ++k) known[k] = true;
    bool scc_known = false;
    const auto is_known = [&](uint32_t s, bool wide) {
      uint32_t c;
      if (s == kLiteral || inline_constant(s, c) || s == kNull) return true;
      if (s == kScc) return scc_known;
      if (s >= 128) return false;
      return known[s] && (!wide || (s + 1 < 128 && known[s + 1]));
    };
    for (uint32_t k = 0; k < insns.size(); ++k) {
      index_ = k;
      if (const auto it = loop_writes.find(insns[k].pc); it != loop_writes.end()) {
        for (uint32_t x = 0; x < 128; ++x)
          if (it->second[x]) known[x] = false;
        scc_known = false;
      }
      const rdna2::Instruction& i = insns[k].insn;
      const std::string_view n = name_of(i);
      const uint32_t w0 = i.words[0], w1 = i.words[1];
      if (i.format == Format::Smem) {
        uint32_t d, c;
        scalar_dsts(i, d, c);
        if (starts(n, "s_load") || starts(n, "s_buffer_load")) {
          const uint32_t sbase = (w0 & 0x3f) * 2, soffset = (w1 >> 25) & 0x7f;
          const bool buffer = starts(n, "s_buffer");
          bool base_known = true;
          for (uint32_t x = sbase; x < sbase + (buffer ? 4 : 2); ++x) base_known = base_known && is_known(x, false);
          const bool offset_known = soffset == kNull || is_known(soffset, false);
          // Loads whose address the CPU cannot compute run on the GPU (which
          // reads guest memory through its copy): a buffer load at a GPU-side
          // offset reads through a CPU-resolved Buffer slot, anything else
          // from the SGPR values themselves.
          if (!base_known) gpu_load_.insert(k);
          else if (!offset_known) (buffer ? gpu_smem_ : gpu_load_).insert(k);
        }
        for (uint32_t x = d; x < d + c && x < 128; ++x)
          known[x] = !loopy[x] && !gpu_smem_.count(k) && !gpu_load_.count(k);
        continue;
      }
      if (i.format == Format::Sop1 || i.format == Format::Sop2 || i.format == Format::Sopk || i.format == Format::Sopc) {
        const bool wide = n.ends_with("_b64") || n.ends_with("_u64") || n.ends_with("_i64");
        bool in = true;
        if (i.format == Format::Sop1) in = is_known(w0 & 0xff, wide && !starts(n, "s_bcnt") && !starts(n, "s_ff"));
        else if (i.format == Format::Sop2 || i.format == Format::Sopc) {
          const bool shift = starts(n, "s_lshl_b64") || starts(n, "s_lshr_b64") || starts(n, "s_ashr_i64") || starts(n, "s_bitcmp");
          in = is_known(w0 & 0xff, wide) && is_known((w0 >> 8) & 0xff, wide && !shift);
        } else if (i.format == Format::Sopk && n != "s_movk_i32") in = is_known((w0 >> 16) & 0x7f, false);
        if (n.find("cselect") != std::string_view::npos || n == "s_addc_u32" || n == "s_subb_u32" || starts(n, "s_cmov"))
          in = in && scc_known;
        if (n.find("exec") != std::string_view::npos) in = false;
        // Only what the CPU interpreter evaluates.
        ScalarRun probe;
        probe.read = [](uint64_t, void*, size_t) { return false; };
        bool pscc = false, pstop = false;
        if (!scalar_step(i, probe, pscc, pstop)) in = false;
        uint32_t d, c;
        scalar_dsts(i, d, c);
        for (uint32_t x = d; x < d + c && x < 128; ++x) known[x] = in && !loopy[x];
        if (i.format == Format::Sopc || n.find("cmpk") != std::string_view::npos ||
            (i.format != Format::Sopk && n != "s_mov_b32" && n != "s_mov_b64" && n != "s_cselect_b32" && n != "s_cselect_b64" &&
             n != "s_brev_b32" && n != "s_movk_i32" && n != "s_mul_i32"))
          scc_known = in;
        continue;
      }
      if (i.format == Format::Mubuf || i.format == Format::Mtbuf || i.format == Format::Mimg) {
        const uint32_t srsrc = ((w1 >> 16) & 0x1f) * 4;
        const uint32_t words = i.format == Format::Mimg ? 8 : 4;
        for (uint32_t x = srsrc; x < srsrc + words; ++x)
          if (!is_known(x, false)) {
            // A buffer descriptor only the GPU knows holds a guest address
            // the GPU can use as it is.
            // An image descriptor only the GPU knows is looked up in the
            // renderer's table of descriptors (desc_lookup).
            gpu_desc_.insert(k);
            break;
          }
        if (i.format == Format::Mimg && (starts(n, "image_sample") || starts(n, "image_gather"))) {
          const uint32_t ssamp = ((w1 >> 21) & 0x1f) * 4;
          for (uint32_t x = ssamp; x < ssamp + 4; ++x)
            if (!is_known(x, false)) {
              gpu_samp_.insert(k);  // the shader derives the sampler from the S# itself
              break;
            }
        }
        continue;
      }
      uint32_t d, c;
      scalar_dsts(i, d, c);
      for (uint32_t x = d; x < d + c && x < 128; ++x) known[x] = false;
    }
    index_ = 0;
    return true;
  }

  // LDS (ds_*): one shared uint array; addresses are byte offsets.
  // global_load/store_dword[xN]: address = VGPR pair (saddr off) or SGPR
  // pair + 32-bit VGPR offset, plus a signed 12-bit immediate.
  bool emit_global(const rdna2::Instruction& insn) {
    const std::string_view n = name_of(insn);
    const uint32_t w0 = insn.words[0], w1 = insn.words[1];
    const int32_t imm = static_cast<int32_t>(w0 << 20) >> 20;
    const uint32_t addr = w1 & 0xff, data = (w1 >> 8) & 0xff, saddr = (w1 >> 16) & 0x7f, vdst = w1 >> 24;
    const bool load = starts(n, "global_load_dword"), store = starts(n, "global_store_dword");
    if (!load && !store) return fail("");
    const uint32_t words = n.ends_with("x4") ? 4 : n.ends_with("x3") ? 3 : n.ends_with("x2") ? 2 : 1;
    std::string base;
    if (saddr == 0x7d || saddr == 0x7f) {
      base = std::format("packUint2x32(uvec2({}, {}))", vreg(addr), vreg(addr + 1));
    } else {
      std::string s;
      if (!src64(saddr, insn, s)) return false;
      base = std::format("({} + uint64_t({}))", s, vreg(addr));
    }
    line(std::format("{{ uint64_t ga = {} + uint64_t(int64_t({}));", base, imm));
    for (uint32_t k = 0; k < words; ++k) {
      if (load) line(std::format("  {} = ld32(ga + {}ul);", vreg(vdst + k), k * 4));
      else line(std::format("  st32(ga + {}ul, {});", k * 4, vreg(data + k)));
    }
    line("}");
    return true;
  }

  bool emit_ds(const rdna2::Instruction& insn) {
    const std::string_view n = name_of(insn);
    const uint32_t w0 = insn.words[0], w1 = insn.words[1];
    const uint32_t offset0 = w0 & 0xff, offset1 = (w0 >> 8) & 0xff, offset = w0 & 0xffff;
    const uint32_t addr = w1 & 0xff, data0 = (w1 >> 8) & 0xff, data1 = (w1 >> 16) & 0xff, vdst = (w1 >> 24) & 0xff;
    const std::string A = vreg(addr);
    lds_words_ = std::max<uint32_t>(lds_words_, 1);
    const auto at = [&](uint32_t off, const std::string& extra = "") {
      return std::format("lds[(({} + {}u{}) >> 2) & LDS_MASK]", A, off, extra);
    };
    const auto two = [&](uint32_t scale, int words, bool write) {
      for (int w = 0; w < words; ++w) {
        const std::string lo = std::format(" + {}u", w * 4);
        if (write) {
          line(std::format("{} = {};", at(offset0 * scale, lo), vreg(data0 + w)));
        } else {
          line(std::format("{} = {};", vreg(vdst + w), at(offset0 * scale, lo)));
        }
      }
      for (int w = 0; w < words; ++w) {
        const std::string lo = std::format(" + {}u", w * 4);
        if (write) line(std::format("{} = {};", at(offset1 * scale, lo), vreg(data1 + w)));
        else line(std::format("{} = {};", vreg(vdst + words + w), at(offset1 * scale, lo)));
      }
      return true;
    };
    if (n == "ds_read_b32") return line(std::format("{} = {};", vreg(vdst), at(offset))), true;
    if (n == "ds_read_b64" || n == "ds_read_b96" || n == "ds_read_b128") {
      const int words = n == "ds_read_b64" ? 2 : n == "ds_read_b96" ? 3 : 4;
      for (int w = 0; w < words; ++w) line(std::format("{} = {};", vreg(vdst + w), at(offset, std::format(" + {}u", w * 4))));
      return true;
    }
    if (n == "ds_write_b32") return line(std::format("{} = {};", at(offset), vreg(data0))), true;
    if (n == "ds_write_b64" || n == "ds_write_b96" || n == "ds_write_b128") {
      const int words = n == "ds_write_b64" ? 2 : n == "ds_write_b96" ? 3 : 4;
      for (int w = 0; w < words; ++w) line(std::format("{} = {};", at(offset, std::format(" + {}u", w * 4)), vreg(data0 + w)));
      return true;
    }
    if (n == "ds_write_b8") return line(std::format("{{ uint i = ({} + {}u); uint s = (i & 3u) * 8u; atomicAnd(lds[(i >> 2) & LDS_MASK], ~(0xffu << s)); atomicOr(lds[(i >> 2) & LDS_MASK], ({} & 0xffu) << s); }}", A, offset, vreg(data0))), true;
    if (n == "ds_write_b16") return line(std::format("{{ uint i = ({} + {}u); uint s = (i & 2u) * 8u; atomicAnd(lds[(i >> 2) & LDS_MASK], ~(0xffffu << s)); atomicOr(lds[(i >> 2) & LDS_MASK], ({} & 0xffffu) << s); }}", A, offset, vreg(data0))), true;
    if (n == "ds_read_u8" || n == "ds_read_u16" || n == "ds_read_i8" || n == "ds_read_i16") {
      const int bits = n.ends_with("8") ? 8 : 16;
      const bool sign = n[8] == 'i';
      line(std::format("{{ uint i = ({} + {}u); uint w = lds[(i >> 2) & LDS_MASK]; {} = {}; }}", A, offset, vreg(vdst),
                       sign ? std::format("uint(bitfieldExtract(int(w), int((i & 3u) * 8u), {}))", bits)
                            : std::format("bitfieldExtract(w, int((i & 3u) * 8u), {})", bits)));
      return true;
    }
    if (n == "ds_read2_b32") return two(4, 1, false);
    if (n == "ds_read2_b64") return two(8, 2, false);
    if (n == "ds_read2st64_b32") return two(256, 1, false);
    if (n == "ds_read2st64_b64") return two(512, 2, false);
    if (n == "ds_write2_b32") return two(4, 1, true);
    if (n == "ds_write2_b64") return two(8, 2, true);
    if (n == "ds_write2st64_b32") return two(256, 1, true);
    if (n == "ds_write2st64_b64") return two(512, 2, true);
    // atomics: ds_<op>_u32 / _rtn_u32
    static const std::pair<std::string_view, const char*> kAtomics[] = {
        {"add", "atomicAdd"}, {"sub", ""}, {"min_u32", "atomicMin"}, {"max_u32", "atomicMax"},
        {"min_i32", ""}, {"max_i32", ""}, {"and", "atomicAnd"}, {"or", "atomicOr"}, {"xor", "atomicXor"},
        {"wrxchg", "atomicExchange"}};
    std::string_view rest = n.substr(3);
    const bool rtn = rest.find("_rtn") != std::string_view::npos;
    std::string op(rest.substr(0, rest.find('_')));
    if (op == "min" || op == "max") op += std::string(rest.substr(rest.size() - 4));
    for (const auto& [k, f] : kAtomics) {
      if (op != k) continue;
      const std::string place = at(offset);
      std::string call;
      if (op == "sub") call = std::format("atomicAdd({}, uint(-int({})))", place, vreg(data0));
      else if (op == "min_i32" || op == "max_i32") return fail("signed LDS min/max");
      else call = std::format("{}({}, {})", f, place, vreg(data0));
      if (rtn || op == "wrxchg") line(std::format("{} = {};", vreg(vdst), call));
      else line(call + ";");
      return true;
    }
    if (n == "ds_cmpst_rtn_b32" || n == "ds_cmpst_b32") {
      line(std::format("{{ uint r = atomicCompSwap({}, {}, {});{} }}", at(offset), vreg(data0), vreg(data1),
                       n == "ds_cmpst_rtn_b32" ? std::format(" {} = r;", vreg(vdst)) : ""));
      return true;
    }
    if (n == "ds_nop") return true;
    return fail("");
  }

  void finish_wave() {
    const ComputeInfo& c = out_.compute;
    std::string g = "#version 460\n"
                    "#extension GL_EXT_buffer_reference : require\n"
                    "#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require\n"
                    "#extension GL_EXT_nonuniform_qualifier : require\n"
                    "#extension GL_KHR_shader_subgroup_basic : require\n"
                    "#extension GL_KHR_shader_subgroup_ballot : require\n"
                    "#extension GL_KHR_shader_subgroup_shuffle : require\n"
                    "#extension GL_EXT_shader_image_load_formatted : enable\n";
    g += std::format("layout(local_size_x = {}, local_size_y = {}, local_size_z = {}) in;\n", c.local_size[0],
                     c.local_size[1], c.local_size[2]);
    g += "layout(buffer_reference, std430, buffer_reference_align = 4) buffer Words { uint w[]; };\n"
         "layout(push_constant) uniform Push { uint64_t data; uint64_t mem; } pc;\n";
    g += mem::glsl_helpers(true);
    g += ""
         "layout(set = 0, binding = 0) uniform texture2D textures[];\n"
         "layout(set = 0, binding = 2) uniform texture2DArray textures_a[];\n"
         "layout(set = 0, binding = 3) uniform texture3D textures_3d[];\n"
         "layout(set = 0, binding = 1) uniform sampler samplers[];\n"
         "layout(set = 0, binding = 4) uniform image2D images[];\n"
         "layout(set = 0, binding = 5) uniform image2DArray images_a[];\n"
         "layout(set = 0, binding = 6) uniform image3D images_3d[];\n"
         "uint D(uint i) { return Words(pc.data).w[i]; }\n" + std::string(kCmpDepth) +
         "uint ld32(uint64_t a) {\n"
         "  uint64_t al = a & ~uint64_t(3); uint sh = uint(a & uint64_t(3)) * 8u;\n"
         "  uint w0 = Words(gpa(al)).w[0];\n"
         "  if (sh == 0u) return w0;\n"
         "  return (w0 >> sh) | (Words(gpa(al + 4ul)).w[0] << (32u - sh));\n"
         "}\n"
         "void st32(uint64_t a, uint v) { Words(gpw(a & ~uint64_t(3))).w[0] = v; }\n";
    if (uses_fetch_format_)
      // gfx10 buffer formats: (bits per component, components, kind) - unorm,
      // snorm, uint, sint, float; out-of-range reads give (0, 0, 0, 1).
      g += "uvec4 fetch_format(uint64_t a, uint f, bool inb) {\n"
           "  uint bits = 0u, n = 0u, kind = 0u;\n"  // kind: 0 unorm 1 snorm 2 uint 3 sint 4 float
           "  switch (f) {\n"
           "    case 1u: bits = 8u; n = 1u; kind = 0u; break;   case 2u: bits = 8u; n = 1u; kind = 1u; break;\n"
           "    case 5u: bits = 8u; n = 1u; kind = 2u; break;   case 6u: bits = 8u; n = 1u; kind = 3u; break;\n"
           "    case 7u: bits = 16u; n = 1u; kind = 0u; break;  case 8u: bits = 16u; n = 1u; kind = 1u; break;\n"
           "    case 11u: bits = 16u; n = 1u; kind = 2u; break; case 12u: bits = 16u; n = 1u; kind = 3u; break;\n"
           "    case 13u: bits = 16u; n = 1u; kind = 4u; break;\n"
           "    case 14u: bits = 8u; n = 2u; kind = 0u; break;  case 15u: bits = 8u; n = 2u; kind = 1u; break;\n"
           "    case 18u: bits = 8u; n = 2u; kind = 2u; break;  case 19u: bits = 8u; n = 2u; kind = 3u; break;\n"
           "    case 20u: bits = 32u; n = 1u; kind = 2u; break; case 21u: bits = 32u; n = 1u; kind = 3u; break;\n"
           "    case 22u: bits = 32u; n = 1u; kind = 4u; break;\n"
           "    case 23u: bits = 16u; n = 2u; kind = 0u; break; case 24u: bits = 16u; n = 2u; kind = 1u; break;\n"
           "    case 27u: bits = 16u; n = 2u; kind = 2u; break; case 28u: bits = 16u; n = 2u; kind = 3u; break;\n"
           "    case 29u: bits = 16u; n = 2u; kind = 4u; break;\n"
           "    case 56u: bits = 8u; n = 4u; kind = 0u; break;  case 57u: bits = 8u; n = 4u; kind = 1u; break;\n"
           "    case 60u: bits = 8u; n = 4u; kind = 2u; break;  case 61u: bits = 8u; n = 4u; kind = 3u; break;\n"
           "    case 62u: bits = 32u; n = 2u; kind = 2u; break; case 63u: bits = 32u; n = 2u; kind = 3u; break;\n"
           "    case 64u: bits = 32u; n = 2u; kind = 4u; break;\n"
           "    case 65u: bits = 16u; n = 4u; kind = 0u; break; case 66u: bits = 16u; n = 4u; kind = 1u; break;\n"
           "    case 69u: bits = 16u; n = 4u; kind = 2u; break; case 70u: bits = 16u; n = 4u; kind = 3u; break;\n"
           "    case 71u: bits = 16u; n = 4u; kind = 4u; break;\n"
           "    case 72u: bits = 32u; n = 3u; kind = 2u; break; case 73u: bits = 32u; n = 3u; kind = 3u; break;\n"
           "    case 74u: bits = 32u; n = 3u; kind = 4u; break;\n"
           "    case 75u: bits = 32u; n = 4u; kind = 2u; break; case 76u: bits = 32u; n = 4u; kind = 3u; break;\n"
           "    case 77u: bits = 32u; n = 4u; kind = 4u; break;\n"
           "    default: bits = 32u; n = 1u; kind = 2u; break;\n"
           "  }\n"
           "  bool ints = kind == 2u || kind == 3u;\n"
           "  uvec4 r = uvec4(0u, 0u, 0u, ints ? 1u : 0x3f800000u);\n"
           "  if (!inb) return r;\n"
           "  for (uint i = 0u; i < n; ++i) {\n"
           "    uint raw;\n"
           "    if (bits == 32u) raw = ld32(a + uint64_t(i * 4u));\n"
           "    else { uint byte_at = i * bits / 8u; raw = bitfieldExtract(ld32(a + uint64_t(byte_at)), 0, int(bits)); }\n"
           "    uint v;\n"
           "    if (kind == 0u) v = floatBitsToUint(float(raw) / float((1u << bits) - 1u));\n"
           "    else if (kind == 1u) v = floatBitsToUint(max(float(bitfieldExtract(int(raw), 0, int(bits))) / float((1u << (bits - 1u)) - 1u), -1.0));\n"
           "    else if (kind == 2u) v = raw;\n"
           "    else if (kind == 3u) v = uint(bitfieldExtract(int(raw), 0, int(bits)));\n"
           "    else v = bits == 16u ? floatBitsToUint(unpackHalf2x16(raw).x) : raw;\n"
           "    r[i] = v;\n"
           "  }\n"
           "  return r;\n"
           "}\n";
    if (lds_words_) {
      const uint32_t words = std::min<uint32_t>(std::max<uint32_t>(c.lds_bytes / 4, 1024), 8192);
      uint32_t pow2 = 1;
      while (pow2 < words) pow2 <<= 1;
      g += std::format("#define LDS_MASK {}u\nshared uint lds[{}];\n", pow2 - 1, pow2);
    }
    g += "uint lane;\nuint exec_lo, exec_hi, vcc_lo, vcc_hi, m0, sink, wave_lanes, dppv;\nbool scc, cmpv;\n";
    for (uint32_t k = 0; k < max_sgpr_; ++k) g += std::format("uint s{};\n", k);
    const uint32_t vgprs = std::max<uint32_t>(max_vgpr_, 3);
    for (uint32_t k = 0; k < vgprs; ++k) g += std::format("uint v{};\n", k);
    g += "bool lane_on() { return ((exec_lo >> lane) & 1u) != 0u; }\n";
    g += "void main() {\n"
         "  lane = gl_SubgroupInvocationID;\n"
         "  wave_lanes = subgroupBallot(true).x;\n"
         "  exec_lo = wave_lanes; exec_hi = 0u; vcc_lo = 0u; vcc_hi = 0u; m0 = 0u; scc = false; cmpv = false;\n";
    for (uint32_t k = 0; k < max_sgpr_; ++k) g += std::format("  s{} = 0u;\n", k);
    for (uint32_t k = 0; k < vgprs; ++k) g += std::format("  v{} = 0u;\n", k);
    g += "  v0 = gl_LocalInvocationID.x;\n";
    if (c.tidig_comps >= 1) g += "  v1 = gl_LocalInvocationID.y;\n";
    if (c.tidig_comps >= 2) g += "  v2 = gl_LocalInvocationID.z;\n";
    g += body_;
    g += "}\n";
    out_.glsl = std::move(g);
  }

 private:
  bool fail(std::string_view what) {
    const auto& d = out_.program.instructions[index_];
    out_.error = std::format("unsupported at +{:#x}: {}", d.pc, rdna2::disassemble(d.insn, d.pc));
    if (!what.empty()) out_.error += std::format(" ({})", what);
    return false;
  }

  uint32_t add_slot(SlotKind kind, uint32_t sgpr, uint32_t words) {
    Slot s{kind, index_, sgpr, out_.slot_words};
    out_.slots.push_back(s);
    out_.slot_words += words;
    return s.offset;
  }

  std::string vreg(uint32_t n) {
    max_vgpr_ = std::max(max_vgpr_, n + 1);
    return std::format("v{}", n);
  }
  // Wave mode: SGPRs are shader variables (every lane computes the same value).
  std::string sreg(uint32_t n) {
    if (n >= 106) {
      if (n == kVccLo) return "vcc_lo";
      if (n == kVccHi) return "vcc_hi";
      if (n == kExecLo) return "exec_lo";
      if (n == kExecHi) return "exec_hi";
      if (n == kM0) return "m0";
      return "sink";
    }
    max_sgpr_ = std::max(max_sgpr_, n + 1);
    return std::format("s{}", n);
  }
  // Wave mode: is this lane's bit set in a 32-bit lane mask?
  static std::string lane_bit(const std::string& mask) { return std::format("(((({}) >> lane) & 1u) != 0u)", mask); }

  // A source operand as a uint expression.
  bool src(uint32_t s, const rdna2::Instruction& insn, std::string& e) {
    uint32_t c;
    if (s == kSdwaSrc0 || s == kSdwaSrc1) return e = std::format("sdwa{}_{}", s - kSdwaSrc0, index_), true;
    if (s >= 256) return e = vreg(s - 256), true;
    if (s == kLiteral) return e = std::format("{:#x}u", insn.literal), true;
    if (inline_constant(s, c)) return e = std::format("{:#x}u", c), true;
    if (wave_) {
      if (s < 106) return e = sreg(s), true;
      switch (s) {
        case kVccLo: return e = "vcc_lo", true;
        case kVccHi: return e = "vcc_hi", true;
        case kExecLo: return e = "exec_lo", true;
        case kExecHi: return e = "exec_hi", true;
        case kM0: return e = "m0", true;
        case kNull: return e = "0u", true;
        case kScc: return e = "uint(scc)", true;
        case 251: return e = "uint(vcc_lo == 0u)", true;   // src_vccz
        case 252: return e = "uint(exec_lo == 0u)", true;  // src_execz
        case 250: return dpp_ ? (e = "dppv", true) : fail("DPP outside wave mode");
        default: return fail(std::format("operand {}", s));
      }
    }
    if (s == kVccLo) return e = "(vcc ? 1u : 0u)", true;
    if (s < 106 && dyn_[s]) return e = std::format("sd{}", s), true;
    if (is_sgpr_operand(s) && s != kNull) {
      // A scalar value: captured per draw (exec reads come out as all lanes).
      if (s == kExecLo || s == kExecHi) return e = "0xffffffffu", true;
      e = std::format("D({})", add_slot(SlotKind::Sgpr, s, 1));
      return true;
    }
    if (s == kNull) return e = "0u", true;
    return fail(std::format("operand {}", s));
  }

  // A float source with VOP3 abs/neg modifiers.
  bool fsrc(const Vop& v, int k, const rdna2::Instruction& insn, std::string& e) {
    std::string u;
    if (!src(v.src[k], insn, u)) return false;
    e = std::format("uintBitsToFloat({})", u);
    if (v.abs & (1u << k)) e = std::format("abs({})", e);
    if (v.neg & (1u << k)) e = std::format("(-{})", e);
    return true;
  }

  std::string fresult(const Vop& v, const std::string& expr) {
    std::string r = expr;
    if (v.omod == 1) r = std::format("({}) * 2.0", r);
    else if (v.omod == 2) r = std::format("({}) * 4.0", r);
    else if (v.omod == 3) r = std::format("({}) * 0.5", r);
    if (v.clamp) r = std::format("clamp({}, 0.0, 1.0)", r);
    return std::format("floatBitsToUint({})", r);
  }

  void line(const std::string& s) {
    body_ += "  ";
    body_ += s;
    body_ += '\n';
  }

  // SDWA: the selected bytes/words of the sources go into temporaries the
  // operation reads; a destination select merges the result into the
  // register afterwards.
  bool emit_valu(const rdna2::Instruction& insn) {
    Vop v = vop_fields(insn);
    if (!v.sdwa) return emit_valu_op(insn, v);
    const std::string_view n = name_of(insn);
    // Float sources (a conversion from an integer reads an integer).
    const bool is_float = (n.find("_f32") != std::string_view::npos || n.find("_f16") != std::string_view::npos) &&
                          !(starts(n, "v_cvt_f32_") && (n.ends_with("_u32") || n.ends_with("_i32") || n.find("ubyte") != std::string_view::npos)) &&
                          !(starts(n, "v_cvt_f16_") && (n.ends_with("_u16") || n.ends_with("_i16")));
    // A half-precision operation reads its operand from the selected word;
    // v_cmp(x)_class's second operand is an integer mask.
    const bool f16_source = n.ends_with("_f16");
    for (int k = 0; k < (insn.format == Format::Vop1 ? 1 : 2); ++k) {
      const bool float_operand = is_float && !(k == 1 && (starts(n, "v_cmp_class") || starts(n, "v_cmpx_class")));
      if (float_operand && !f16_source && v.sel[k] != 6) return fail("SDWA sub-dword float operand");
      std::string u;
      if (!src(v.src[k], insn, u)) return false;
      const uint32_t sel = v.sel[k];
      std::string e = u;
      if (sel < 4) e = v.sext[k] ? std::format("uint(bitfieldExtract(int({}), {}, 8))", u, sel * 8)
                                 : std::format("bitfieldExtract({}, {}, 8)", u, sel * 8);
      else if (sel < 6) e = v.sext[k] ? std::format("uint(bitfieldExtract(int({}), {}, 16))", u, (sel - 4) * 16)
                                      : std::format("bitfieldExtract({}, {}, 16)", u, (sel - 4) * 16);
      else if (sel != 6) return fail("SDWA select");
      line(std::format("uint sdwa{}_{} = {};", k, index_, e));
      v.src[k] = kSdwaSrc0 + k;
    }
    if (insn.format == Format::Vopc) {
      if (v.sdst != kVccLo && !wave_) return fail("SDWA compare into an SGPR pair");
      return emit_valu_op(insn, v);
    }
    if (v.dst_sel == 6) return emit_valu_op(insn, v);
    if (v.dst_sel > 6 || v.dst_unused == 1) return fail("SDWA destination select");
    const std::string d = vreg(v.dst);
    line(std::format("uint sdwa_old_{} = {};", index_, d));
    if (!emit_valu_op(insn, v)) return false;
    const uint32_t offset = v.dst_sel < 4 ? v.dst_sel * 8 : (v.dst_sel - 4) * 16, bits = v.dst_sel < 4 ? 8 : 16;
    line(std::format("{} = bitfieldInsert({}, {}, {}, {});", d, v.dst_unused == 2 ? std::format("sdwa_old_{}", index_) : "0u",
                     d, offset, bits));
    return true;
  }

  bool emit_valu_op(const rdna2::Instruction& insn, Vop v) {
    const std::string_view n = name_of(insn);
    if (n == "v_nop") return true;
    std::string a, b, c;
    const auto f2 = [&](const char* op) -> bool {
      if (!fsrc(v, 0, insn, a) || !fsrc(v, 1, insn, b)) return false;
      line(std::format("{} = {};", vreg(v.dst), fresult(v, std::format("{} {} {}", a, op, b))));
      return true;
    };
    const auto u2 = [&](const char* fmt) -> bool {
      if (!src(v.src[0], insn, a) || !src(v.src[1], insn, b)) return false;
      line(std::format("{} = {};", vreg(v.dst), std::vformat(fmt, std::make_format_args(a, b))));
      return true;
    };
    if (n == "v_mov_b32") {
      if (!src(v.src[0], insn, a)) return false;
      line(std::format("{} = {};", vreg(v.dst), a));
      return true;
    }
    if (n == "v_add_f32") return f2("+");
    if (n == "v_sub_f32") return f2("-");
    if (n == "v_subrev_f32") {
      std::swap(v.src[0], v.src[1]);
      return f2("-");
    }
    if (n == "v_mul_f32" || n == "v_mul_legacy_f32") return f2("*");
    if (n == "v_max_f32" || n == "v_min_f32") {
      if (!fsrc(v, 0, insn, a) || !fsrc(v, 1, insn, b)) return false;
      line(std::format("{} = {};", vreg(v.dst),
                       fresult(v, std::format("{}({}, {})", n == "v_max_f32" ? "max" : "min", a, b))));
      return true;
    }
    if (n == "v_fma_f32" || n == "v_mad_f32" || n == "v_fma_legacy_f32") {
      if (!fsrc(v, 0, insn, a) || !fsrc(v, 1, insn, b) || !fsrc(v, 2, insn, c)) return false;
      line(std::format("{} = {};", vreg(v.dst), fresult(v, std::format("fma({}, {}, {})", a, b, c))));
      return true;
    }
    if (n == "v_fmac_f32" || n == "v_mac_f32") {
      if (!fsrc(v, 0, insn, a) || !fsrc(v, 1, insn, b)) return false;
      line(std::format("{} = {};", vreg(v.dst),
                       fresult(v, std::format("fma({}, {}, uintBitsToFloat({}))", a, b, vreg(v.dst)))));
      return true;
    }
    if (n == "v_fmaak_f32" || n == "v_fmamk_f32") {
      // fmaak: s0 * s1 + K; fmamk: s0 * K + s1 (K = the literal)
      if (!fsrc(v, 0, insn, a) || !fsrc(v, 1, insn, b)) return false;
      const std::string k = std::format("uintBitsToFloat({:#x}u)", insn.literal);
      const std::string e = n == "v_fmaak_f32" ? std::format("fma({}, {}, {})", a, b, k) : std::format("fma({}, {}, {})", a, k, b);
      line(std::format("{} = {};", vreg(v.dst), fresult(v, e)));
      return true;
    }
    if (n == "v_add_nc_u32" || n == "v_add_u32") return u2("{} + {}");
    if (n == "v_sub_nc_u32") return u2("{} - {}");
    if (n == "v_subrev_nc_u32") return u2("{1} - {0}");
    if (n == "v_swap_b32" && wave_) {
      if (!src(v.src[0], insn, a)) return false;
      line(std::format("{{ uint t = {}; {} = {}; {} = t; }}", vreg(v.dst), vreg(v.dst), a, a));
      return true;
    }
    if (n == "v_and_b32") return u2("{} & {}");
    if (n == "v_or_b32") return u2("{} | {}");
    if (n == "v_xor_b32") return u2("{} ^ {}");
    if (n == "v_lshlrev_b32") return u2("{1} << ({0} & 31u)");
    if (n == "v_lshrrev_b32") return u2("{1} >> ({0} & 31u)");
    if (n == "v_ashrrev_i32") return u2("uint(int({1}) >> int({0} & 31u))");
    if (n == "v_mul_u32_u24") return u2("({} & 0xffffffu) * ({} & 0xffffffu)");
    if (n == "v_max_u32") return u2("max({}, {})");
    if (n == "v_min_u32") return u2("min({}, {})");
    if (n == "v_and_or_b32" || n == "v_lshl_or_b32" || n == "v_lshl_add_u32" || n == "v_add3_u32" ||
        n == "v_or3_b32" || n == "v_bfe_u32" || n == "v_cndmask_b32" || n == "v_mad_u32_u24") {
      if (!src(v.src[0], insn, a) || !src(v.src[1], insn, b)) return false;
      if (n == "v_cndmask_b32") {
        std::string cond = "vcc";
        if (wave_) {
          std::string m = "vcc_lo";
          if (insn.format == Format::Vop3 && !src(v.src[2], insn, m)) return false;
          cond = lane_bit(m);
          if (insn.format == Format::Vop3) {  // VOP3 abs/neg on the float sources
            if (v.neg & 1) a = std::format("({} ^ 0x80000000u)", a);
            if (v.neg & 2) b = std::format("({} ^ 0x80000000u)", b);
            if (v.abs & 1) a = std::format("({} & 0x7fffffffu)", a);
            if (v.abs & 2) b = std::format("({} & 0x7fffffffu)", b);
          }
        } else if (insn.format == Format::Vop3) {
          if (v.src[2] != kVccLo) return fail("v_cndmask with a condition other than vcc");
        }
        line(std::format("{} = {} ? {} : {};", vreg(v.dst), cond, b, a));
        return true;
      }
      if (!src(v.src[2], insn, c)) return false;
      std::string e;
      if (n == "v_and_or_b32") e = std::format("({} & {}) | {}", a, b, c);
      else if (n == "v_lshl_or_b32") e = std::format("({} << ({} & 31u)) | {}", a, b, c);
      else if (n == "v_lshl_add_u32") e = std::format("({} << ({} & 31u)) + {}", a, b, c);
      else if (n == "v_add3_u32") e = std::format("{} + {} + {}", a, b, c);
      else if (n == "v_or3_b32") e = std::format("{} | {} | {}", a, b, c);
      else if (n == "v_mad_u32_u24") e = std::format("({} & 0xffffffu) * ({} & 0xffffffu) + {}", a, b, c);
      else e = std::format("bitfieldExtract({}, int({} & 31u), int({} & 31u))", a, b, c);
      line(std::format("{} = {};", vreg(v.dst), e));
      return true;
    }
    // conversions and unary float
    const auto f1 = [&](const char* fmt, bool float_result) -> bool {
      if (!fsrc(v, 0, insn, a)) return false;
      const std::string e = std::vformat(fmt, std::make_format_args(a));
      line(std::format("{} = {};", vreg(v.dst), float_result ? fresult(v, e) : e));
      return true;
    };
    if (n == "v_cvt_i32_f32") return f1("uint(int({}))", false);
    if (n == "v_cvt_u32_f32") return f1("uint(max({}, 0.0))", false);
    if (n == "v_cvt_flr_i32_f32") return f1("uint(int(floor({})))", false);
    if (n == "v_cvt_rpi_i32_f32") return f1("uint(int(floor({} + 0.5)))", false);
    if (n == "v_sqrt_f32") return f1("sqrt({})", true);
    if (n == "v_rsq_f32") return f1("inversesqrt({})", true);
    if (n == "v_rcp_f32") return f1("1.0 / {}", true);
    if (n == "v_exp_f32") return f1("exp2({})", true);
    if (n == "v_log_f32") return f1("log2({})", true);
    if (n == "v_floor_f32") return f1("floor({})", true);
    if (n == "v_ceil_f32") return f1("ceil({})", true);
    if (n == "v_fract_f32") return f1("fract({})", true);
    if (n == "v_trunc_f32") return f1("trunc({})", true);
    if (n == "v_rndne_f32") return f1("roundEven({})", true);
    if (n == "v_sin_f32") return f1("sin({} * 6.283185307)", true);
    if (n == "v_cos_f32") return f1("cos({} * 6.283185307)", true);
    if (n == "v_cvt_f32_i32" || n == "v_cvt_f32_u32") {
      if (!src(v.src[0], insn, a)) return false;
      line(std::format("{} = {};", vreg(v.dst),
                       fresult(v, std::format("float({}({}))", n == "v_cvt_f32_i32" ? "int" : "uint", a))));
      return true;
    }
    if (n == "v_cvt_f32_f16") {
      if (!src(v.src[0], insn, a)) return false;
      line(std::format("{} = {};", vreg(v.dst), fresult(v, std::format("unpackHalf2x16({}).x", a))));
      return true;
    }
    if (n == "v_cvt_f16_f32") {
      if (!fsrc(v, 0, insn, a)) return false;
      line(std::format("{} = ({} & 0xffff0000u) | (packHalf2x16(vec2({}, 0.0)) & 0xffffu);", vreg(v.dst),
                       vreg(v.dst), a));
      return true;
    }
    if (n == "v_cvt_pkrtz_f16_f32") {
      if (!fsrc(v, 0, insn, a) || !fsrc(v, 1, insn, b)) return false;
      line(std::format("{} = packHalf2x16(vec2({}, {}));", vreg(v.dst), a, b));
      return true;
    }
    if (n == "v_fma_mixlo_f16" || n == "v_fma_mixhi_f16" || n == "v_fma_mix_f32") {
      std::string s[3];
      for (int k = 0; k < 3; ++k) {
        std::string u;
        if (!src(v.src[k], insn, u)) return false;
        if (v.opsel_hi & (1u << k))  // f16 operand, half selected by op_sel
          s[k] = std::format("unpackHalf2x16({}).{}", u, (v.opsel & (1u << k)) ? 'y' : 'x');
        else
          s[k] = std::format("uintBitsToFloat({})", u);
        if (v.neg & (1u << k)) s[k] = std::format("(-{})", s[k]);
      }
      std::string r = std::format("fma({}, {}, {})", s[0], s[1], s[2]);
      if (v.clamp) r = std::format("clamp({}, 0.0, 1.0)", r);
      if (n == "v_fma_mix_f32") {
        line(std::format("{} = floatBitsToUint({});", vreg(v.dst), r));
      } else if (n == "v_fma_mixlo_f16") {
        line(std::format("{} = ({} & 0xffff0000u) | (packHalf2x16(vec2({}, 0.0)) & 0xffffu);", vreg(v.dst),
                         vreg(v.dst), r));
      } else {
        line(std::format("{} = ({} & 0xffffu) | (packHalf2x16(vec2({}, 0.0)) << 16);", vreg(v.dst), vreg(v.dst), r));
      }
      return true;
    }
    // Integer add/sub with carry: the carry is a lane mask (vcc, or an SGPR
    // pair in the VOP3 form); carry-in reads this lane's bit.
    if (wave_ && (n == "v_add_co_u32" || n == "v_sub_co_u32" || n == "v_subrev_co_u32" || n == "v_add_co_ci_u32" ||
                  n == "v_sub_co_ci_u32" || n == "v_subrev_co_ci_u32")) {
      if (!src(v.src[0], insn, a) || !src(v.src[1], insn, b)) return false;
      const bool ci = n.ends_with("_ci_u32");
      std::string cin = "0u";
      if (ci) {
        std::string m = "vcc_lo";
        if (insn.format == Format::Vop3 && !src(v.src[2], insn, m)) return false;
        cin = std::format("uint({})", lane_bit(m));
      }
      if (n.find("subrev") != std::string_view::npos) std::swap(a, b);
      const bool sub = n.find("sub") != std::string_view::npos;
      if (sub) line(std::format("{{ uint64_t w = uint64_t({}) - uint64_t({}) - uint64_t({}); {} = uint(w); cmpv = (w >> 32) != uint64_t(0); }}", a, b, cin, vreg(v.dst)));
      else line(std::format("{{ uint64_t w = uint64_t({}) + uint64_t({}) + uint64_t({}); {} = uint(w); cmpv = (w >> 32) != uint64_t(0); }}", a, b, cin, vreg(v.dst)));
      const uint32_t sdst = insn.format == Format::Vop3 ? v.sdst : kVccLo;
      if (sdst == kVccLo) post_.push_back("vcc_lo = subgroupBallot(cmpv).x; vcc_hi = 0u;");
      else post_.push_back(std::format("{} = subgroupBallot(cmpv).x;", sreg(sdst)));  // wave32: one SGPR
      return true;
    }
    // 64-bit shifts (result in a VGPR pair)
    if (n == "v_lshrrev_b64" || n == "v_lshlrev_b64" || n == "v_ashrrev_i64") {
      std::string lo, hi;
      if (!src(v.src[0], insn, a)) return false;
      if (v.src[1] >= 256) {
        lo = vreg(v.src[1] - 256), hi = vreg(v.src[1] - 255);
      } else if (wave_ && v.src[1] < 106) {
        lo = sreg(v.src[1]), hi = sreg(v.src[1] + 1);
      } else {
        uint32_t cst;
        if (v.src[1] == kLiteral) lo = std::format("{:#x}u", insn.literal), hi = "0u";
        else if (inline_constant(v.src[1], cst)) lo = std::format("{:#x}u", cst), hi = (cst >> 31) ? "0xffffffffu" : "0u";
        else return fail("64-bit shift operand");
      }
      const std::string x = std::format("packUint2x32(uvec2({}, {}))", lo, hi);
      const std::string e = n == "v_lshrrev_b64" ? std::format("{} >> ({} & 63u)", x, a)
                            : n == "v_lshlrev_b64" ? std::format("{} << ({} & 63u)", x, a)
                                                   : std::format("uint64_t(int64_t({}) >> ({} & 63u))", x, a);
      line(std::format("{{ uvec2 r = unpackUint2x32({}); {} = r.x; {} = r.y; }}", e, vreg(v.dst), vreg(v.dst + 1)));
      return true;
    }
    if (n == "v_mul_lo_u32" || n == "v_mul_lo_i32") return u2("{} * {}");
    if (n == "v_mul_hi_u32") return u2("uint((uint64_t({}) * uint64_t({})) >> 32)");
    if (n == "v_mul_hi_i32") return u2("uint((int64_t(int({})) * int64_t(int({}))) >> 32)");
    if (n == "v_mul_i32_i24") return u2("uint(bitfieldExtract(int({}), 0, 24) * bitfieldExtract(int({}), 0, 24))");
    if (n == "v_max_i32") return u2("uint(max(int({}), int({})))");
    if (n == "v_min_i32") return u2("uint(min(int({}), int({})))");
    if (n == "v_not_b32") {
      if (!src(v.src[0], insn, a)) return false;
      line(std::format("{} = ~{};", vreg(v.dst), a));
      return true;
    }
    if (n == "v_bfrev_b32" || n == "v_ffbh_u32" || n == "v_ffbl_b32" || n == "v_ffbh_i32" || n == "v_cvt_f32_ubyte0" ||
        n == "v_cvt_f32_ubyte1" || n == "v_cvt_f32_ubyte2" || n == "v_cvt_f32_ubyte3") {
      if (!src(v.src[0], insn, a)) return false;
      std::string e;
      if (n == "v_bfrev_b32") e = std::format("bitfieldReverse({})", a);
      else if (n == "v_ffbh_u32") e = std::format("({0}) == 0u ? 0xffffffffu : uint(31 - findMSB({0}))", a);
      else if (n == "v_ffbl_b32") e = std::format("uint(findLSB({}))", a);
      else if (n == "v_ffbh_i32") e = std::format("(int({0}) == 0 || int({0}) == -1) ? 0xffffffffu : uint(31 - findMSB(int({0})))", a);
      else e = fresult(v, std::format("float(({} >> {}u) & 0xffu)", a, (n.back() - '0') * 8));
      line(std::format("{} = {};", vreg(v.dst), e));
      return true;
    }
    if (n == "v_bcnt_u32_b32") return u2("uint(bitCount({})) + {}");
    if (n == "v_bfm_b32") return u2("((1u << ({} & 31u)) - 1u) << ({} & 31u)");
    if (n == "v_ldexp_f32") {
      if (!fsrc(v, 0, insn, a) || !src(v.src[1], insn, b)) return false;
      line(std::format("{} = {};", vreg(v.dst), fresult(v, std::format("ldexp({}, clamp(int({}), -256, 256))", a, b))));
      return true;
    }
    if (n == "v_mbcnt_lo_u32_b32" && wave_) return u2("uint(bitCount({} & ((1u << lane) - 1u))) + {}");
    if (n == "v_mbcnt_hi_u32_b32" && wave_) return u2("(0u * {}) + {}");  // wave32: no lanes 32-63
    if (n == "v_add_lshl_u32" || n == "v_lshl_add_u32" || n == "v_bfi_b32" || n == "v_alignbit_b32" ||
        n == "v_med3_u32" || n == "v_med3_i32" || n == "v_min3_u32" || n == "v_max3_u32" || n == "v_min3_i32" ||
        n == "v_max3_i32" || n == "v_alignbyte_b32" || n == "v_xad_u32" || n == "v_xor3_b32" || n == "v_bfe_i32" ||
        n == "v_mad_u32_u16" || n == "v_mad_i32_i24" || n == "v_perm_b32") {
      if (!src(v.src[0], insn, a) || !src(v.src[1], insn, b) || !src(v.src[2], insn, c)) return false;
      std::string e;
      if (n == "v_add_lshl_u32") e = std::format("({} + {}) << ({} & 31u)", a, b, c);
      else if (n == "v_lshl_add_u32") e = std::format("({} << ({} & 31u)) + {}", a, b, c);
      else if (n == "v_bfi_b32") e = std::format("({0} & {1}) | (~{0} & {2})", a, b, c);
      else if (n == "v_alignbit_b32") e = std::format("uint((packUint2x32(uvec2({1}, {0})) >> ({2} & 31u)) & uint64_t(0xffffffffu))", a, b, c);
      else if (n == "v_alignbyte_b32") e = std::format("uint((packUint2x32(uvec2({1}, {0})) >> (({2} & 3u) * 8u)) & uint64_t(0xffffffffu))", a, b, c);
      else if (n == "v_med3_u32") e = std::format("max(min({0}, {1}), min(max({0}, {1}), {2}))", a, b, c);
      else if (n == "v_med3_i32") e = std::format("uint(max(min(int({0}), int({1})), min(max(int({0}), int({1})), int({2}))))", a, b, c);
      else if (n == "v_min3_u32") e = std::format("min(min({}, {}), {})", a, b, c);
      else if (n == "v_max3_u32") e = std::format("max(max({}, {}), {})", a, b, c);
      else if (n == "v_min3_i32") e = std::format("uint(min(min(int({}), int({})), int({})))", a, b, c);
      else if (n == "v_max3_i32") e = std::format("uint(max(max(int({}), int({})), int({})))", a, b, c);
      else if (n == "v_xad_u32") e = std::format("({} ^ {}) + {}", a, b, c);
      else if (n == "v_xor3_b32") e = std::format("{} ^ {} ^ {}", a, b, c);
      else if (n == "v_bfe_i32") e = std::format("uint(bitfieldExtract(int({}), int({} & 31u), int({} & 31u)))", a, b, c);
      else if (n == "v_mad_u32_u16") e = std::format("({} & 0xffffu) * ({} & 0xffffu) + {}", a, b, c);
      else if (n == "v_mad_i32_i24") e = std::format("uint(bitfieldExtract(int({}), 0, 24) * bitfieldExtract(int({}), 0, 24) + int({}))", a, b, c);
      else {  // v_perm_b32: bytes of {a, b} selected by c
        line(std::format("{{ uint64_t pb = packUint2x32(uvec2({1}, {0})); uint sel = {2}, r = 0u;", a, b, c));
        line("  for (uint i = 0u; i < 4u; ++i) { uint q = (sel >> (i * 8u)) & 0xffu; uint byte;");
        line("    if (q < 8u) byte = uint((pb >> (q * 8u)) & uint64_t(0xffu));");
        line("    else if (q == 12u) byte = 0u; else if (q > 12u) byte = 0xffu;");
        line("    else { uint src = uint((pb >> (((q - 8u) * 2u + 1u) * 8u)) & uint64_t(0xffu)); byte = (src & 0x80u) != 0u ? 0xffu : 0u; }");
        line("    r |= byte << (i * 8u); }");
        line(std::format("  {} = r; }}", vreg(v.dst)));
        return true;
      }
      line(std::format("{} = {};", vreg(v.dst), e));
      return true;
    }
    if (n == "v_cubeid_f32" || n == "v_cubema_f32" || n == "v_cubesc_f32" || n == "v_cubetc_f32") {
      if (!fsrc(v, 0, insn, a) || !fsrc(v, 1, insn, b) || !fsrc(v, 2, insn, c)) return false;
      line(std::format("{} = {};", vreg(v.dst), fresult(v, std::format("{}(vec3({}, {}, {}))", n.substr(2, 6), a, b, c))));
      return true;
    }
    if (n == "v_cvt_pk_u8_f32") {
      // Byte (src1 & 3) of src2 = src0 rounded and clamped to [0, 255].
      if (!fsrc(v, 0, insn, a) || !src(v.src[1], insn, b) || !src(v.src[2], insn, c)) return false;
      line(std::format("{} = bitfieldInsert({}, uint(clamp(roundEven({}), 0.0, 255.0)), int(({} & 3u) * 8u), 8);",
                       vreg(v.dst), c, a, b));
      return true;
    }
    if (n == "v_med3_f32" || n == "v_min3_f32" || n == "v_max3_f32") {
      if (!fsrc(v, 0, insn, a) || !fsrc(v, 1, insn, b) || !fsrc(v, 2, insn, c)) return false;
      std::string e;
      if (n == "v_med3_f32") e = std::format("max(min({0}, {1}), min(max({0}, {1}), {2}))", a, b, c);
      else if (n == "v_min3_f32") e = std::format("min(min({}, {}), {})", a, b, c);
      else e = std::format("max(max({}, {}), {})", a, b, c);
      line(std::format("{} = {};", vreg(v.dst), fresult(v, e)));
      return true;
    }
    if (n == "v_cmp_class_f32" || n == "v_cmpx_class_f32" || n == "v_cmp_class_f16" || n == "v_cmpx_class_f16") {
      // class mask: 0 sNaN 1 qNaN 2 -inf 3 -normal 4 -denorm 5 -0 6 +0 7 +denorm 8 +normal 9 +inf
      if (n.ends_with("_f16")) {
        // From the half's bits (abs / neg act on the sign bit).
        if (!src(v.src[0], insn, a) || !src(v.src[1], insn, b)) return false;
        std::string h = std::format("({} & 0xffffu)", a);
        if (v.abs & 1) h = std::format("({} & 0x7fffu)", h);
        if (v.neg & 1) h = std::format("({} ^ 0x8000u)", h);
        line(std::format("{{ uint h = {}; uint m = {}; bool neg = (h >> 15) != 0u; uint e = (h >> 10) & 31u, f = h & 0x3ffu; uint cls;", h, b));
        line("  if (e == 31u) cls = f != 0u ? ((f & 0x200u) != 0u ? 1u : 0u) : (neg ? 2u : 9u);");
        line("  else if (e == 0u) cls = f == 0u ? (neg ? 5u : 6u) : (neg ? 4u : 7u); else cls = neg ? 3u : 8u;");
      } else {
        if (!fsrc(v, 0, insn, a) || !src(v.src[1], insn, b)) return false;
        line(std::format("{{ float x = {}; uint m = {}; uint u = floatBitsToUint(x); bool neg = (u >> 31) != 0u; uint cls;", a, b));
        line("  if (isnan(x)) cls = (u & 0x400000u) != 0u ? 1u : 0u; else if (isinf(x)) cls = neg ? 2u : 9u;");
        line("  else if (x == 0.0) cls = neg ? 5u : 6u; else if ((u & 0x7f800000u) == 0u) cls = neg ? 4u : 7u; else cls = neg ? 3u : 8u;");
      }
      if (wave_) {
        line("  cmpv = ((m >> cls) & 1u) != 0u; }");
        const std::string ballot = "subgroupBallot(cmpv).x";
        if (starts(n, "v_cmpx_")) post_.push_back(std::format("exec_lo = {}; exec_hi = 0u;", ballot));
        else if ((insn.format == Format::Vop3 || v.sdwa) && v.sdst != kVccLo)
          post_.push_back(std::format("{} = {};", sreg(v.sdst), ballot));
        else post_.push_back(std::format("vcc_lo = {}; vcc_hi = 0u;", ballot));
      } else {
        line("  vcc = ((m >> cls) & 1u) != 0u; }");
      }
      return true;
    }
    // compares into vcc (wave mode: also into an SGPR pair, and v_cmpx into exec)
    if (starts(n, "v_cmp_") || (wave_ && starts(n, "v_cmpx_"))) {
      const bool to_exec = starts(n, "v_cmpx_");
      static const std::pair<std::string_view, const char*> kOps[] = {
          {"eq", "=="}, {"lg", "!="}, {"neq", "!="}, {"ne", "!="}, {"lt", "<"},
          {"le", "<="}, {"gt", ">"}, {"ge", ">="}, {"nlt", ">="}, {"nle", ">"},
          {"ngt", "<="}, {"nge", "<"}, {"o", "o"}, {"u", "u"}, {"nlg", "=="}};
      const std::string_view rest = n.substr(to_exec ? 7 : 6);
      const size_t us = rest.find('_');
      if (us == std::string_view::npos) return fail("");
      const std::string_view op = rest.substr(0, us), type = rest.substr(us + 1);
      const char* cmp = nullptr;
      for (const auto& [k, o] : kOps)
        if (op == k) cmp = o;
      if (!cmp && op != "f" && op != "t" && op != "tru") return fail("");
      if (!wave_ && insn.format == Format::Vop3 && v.sdst != kVccLo) return fail("compare into an SGPR pair");
      if (op == "f" || op == "t" || op == "tru") {
        a = b = "0u";
        cmp = op == "f" ? "!=" : "==";
      } else if (type == "f32") {
        if (!fsrc(v, 0, insn, a) || !fsrc(v, 1, insn, b)) return false;
      } else if (type == "u32" || type == "i32") {
        if (!src(v.src[0], insn, a) || !src(v.src[1], insn, b)) return false;
        if (type == "i32") {
          a = std::format("int({})", a);
          b = std::format("int({})", b);
        }
      } else if ((type == "u64" || type == "i64") && wave_) {
        // VGPR pairs or 64-bit scalar operands.
        const auto wide = [&](uint32_t s, std::string& e) {
          if (s >= 256) return e = std::format("packUint2x32(uvec2({}, {}))", vreg(s - 256), vreg(s - 255)), true;
          return src64(s, insn, e);
        };
        if (!wide(v.src[0], a) || !wide(v.src[1], b)) return false;
        if (type == "i64") {
          a = std::format("int64_t({})", a);
          b = std::format("int64_t({})", b);
        }
      } else if (type == "u16" || type == "i16") {
        if (!src(v.src[0], insn, a) || !src(v.src[1], insn, b)) return false;
        a = type == "u16" ? std::format("({} & 0xffffu)", a) : std::format("bitfieldExtract(int({}), 0, 16)", a);
        b = type == "u16" ? std::format("({} & 0xffffu)", b) : std::format("bitfieldExtract(int({}), 0, 16)", b);
      } else if (type == "f16") {
        std::string ua, ub;
        if (!src(v.src[0], insn, ua) || !src(v.src[1], insn, ub)) return false;
        a = std::format("unpackHalf2x16({}).x", ua);
        b = std::format("unpackHalf2x16({}).x", ub);
        if (v.abs & 1) a = std::format("abs({})", a);
        if (v.abs & 2) b = std::format("abs({})", b);
        if (v.neg & 1) a = std::format("(-{})", a);
        if (v.neg & 2) b = std::format("(-{})", b);
      } else {
        return fail("");
      }
      // The n* forms (nlt, nge, ...) are true for unordered operands.
      const bool unordered = op.size() > 1 && op[0] == 'n' && op != "ne" && op != "neq";
      std::string e = std::format("{} {} {}", a, cmp, b);
      if (op == "o") e = std::format("!isnan({}) && !isnan({})", a, b);
      else if (op == "u") e = std::format("isnan({}) || isnan({})", a, b);
      else if (op == "nlg") e = std::format("!({0} < {1} || {0} > {1})", a, b);
      else if (op == "neq" && type == "f32") e = std::format("!({} == {})", a, b);
      else if (unordered) e = std::format("!({} {} {})", a, std::string_view(cmp) == ">=" ? "<" : std::string_view(cmp) == ">" ? "<=" : std::string_view(cmp) == "<=" ? ">" : ">=", b);
      if (!wave_) {
        line(std::format("vcc = {};", e));
        return true;
      }
      line(std::format("cmpv = {};", e));
      const std::string ballot = "subgroupBallot(cmpv).x";
      if (to_exec) post_.push_back(std::format("exec_lo = {}; exec_hi = 0u;", ballot));
      else if ((insn.format == Format::Vop3 || v.sdwa) && v.sdst != kVccLo)
        post_.push_back(std::format("{} = {};", sreg(v.sdst), ballot));
      else post_.push_back(std::format("vcc_lo = {}; vcc_hi = 0u;", ballot));
      return true;
    }
    return fail("");
  }

  bool emit_interp(const rdna2::Instruction& insn) {
    const uint32_t w0 = insn.words[0];
    const uint32_t attr = (w0 >> 10) & 0x3f, chan = (w0 >> 8) & 3, op = (w0 >> 16) & 3, dst = (w0 >> 18) & 0xff;
    out_.inputs = std::max(out_.inputs, attr + 1);
    if (op == 0) return true;  // v_interp_p1_f32: the p2 that follows writes the interpolated value
    if (op == 2) flat_inputs_.insert(attr);  // v_interp_mov_f32
    line(std::format("{} = floatBitsToUint(a_{}.{});", vreg(dst), attr, "xyzw"[chan]));
    return true;
  }

  // Buffer address arithmetic common to MUBUF and MTBUF.
  // `dword3`: the descriptor's dword 3 (format, swizzles). The descriptor is
  // a CPU-resolved Buffer slot, or (gpu_desc_) the V# in the SGPRs.
  bool buffer_address(const rdna2::Instruction& insn, std::string& dword3, std::string& addr, std::string& bound) {
    const uint32_t w0 = insn.words[0], w1 = insn.words[1];
    const uint32_t offset = w0 & 0xfff, offen = (w0 >> 12) & 1, idxen = (w0 >> 13) & 1;
    const uint32_t vaddr = w1 & 0xff, srsrc = ((w1 >> 16) & 0x1f) * 4, soffset = (w1 >> 24) & 0xff;
    std::string lo, hi, stride, records;
    if (gpu_desc_.count(index_)) {
      lo = sreg(srsrc);
      hi = std::format("({} & 0xffffu)", sreg(srsrc + 1));
      stride = std::format("(({} >> 16) & 0x3fffu)", sreg(srsrc + 1));
      records = sreg(srsrc + 2);
      dword3 = sreg(srsrc + 3);
    } else {
      const uint32_t base = add_slot(SlotKind::Buffer, srsrc, 5);
      lo = std::format("D({}u)", base);
      hi = std::format("D({}u)", base + 1);
      stride = std::format("D({}u)", base + 2);
      records = std::format("D({}u)", base + 3);
      dword3 = std::format("D({}u)", base + 4);
    }
    std::string so;
    if (!src(soffset, insn, so)) return false;
    std::string index = idxen ? vreg(vaddr) : "0u";
    std::string voff = offen ? vreg(vaddr + idxen) : "0u";
    addr = std::format("(packUint2x32(uvec2({}, {})) + uint64_t({}) * uint64_t({}) + uint64_t({} + {} + {}u))", lo, hi,
                       index, stride, voff, so, offset);
    bound = idxen ? std::format("({} < {})", index, records) : std::format("({} + {} + {}u < {})", voff, so, offset, records);
    return true;
  }

  bool emit_mubuf(const rdna2::Instruction& insn) {
    const std::string_view n = name_of(insn);
    if (wave_ && (starts(n, "buffer_store") || starts(n, "buffer_atomic"))) return emit_mubuf_write(insn);
    if (wave_ && starts(n, "buffer_load_format_") && !starts(n, "buffer_load_format_d16")) {
      // Typed load: the data format is the descriptor's (dword 3 bits 12-18).
      const int comps = n.ends_with("_xyzw") ? 4 : n.ends_with("_xyz") ? 3 : n.ends_with("_xy") ? 2 : 1;
      const uint32_t vdata = (insn.words[1] >> 8) & 0xff;
      std::string slot, addr, bound;
      if (!buffer_address(insn, slot, addr, bound)) return false;
      line("{");
      line(std::format("  uvec4 t = fetch_format({}, ({} >> 12) & 0x7fu, {});", addr, slot, bound));
      for (int k = 0; k < comps; ++k) line(std::format("  {} = t.{};", vreg(vdata + k), "xyzw"[k]));
      line("}");
      uses_fetch_format_ = true;
      return true;
    }
    int words = 0;
    if (n == "buffer_load_dword") words = 1;
    else if (n == "buffer_load_dwordx2") words = 2;
    else if (n == "buffer_load_dwordx3") words = 3;
    else if (n == "buffer_load_dwordx4") words = 4;
    else return fail("");
    const uint32_t vdata = (insn.words[1] >> 8) & 0xff;
    std::string slot, addr, bound;
    if (!buffer_address(insn, slot, addr, bound)) return false;
    line("{");
    line(std::format("  uint64_t a = {}; bool inb = {};", addr, bound));
    for (int k = 0; k < words; ++k)
      line(std::format("  {} = inb ? ld32(a + {}ul) : 0u;", vreg(vdata + k), k * 4));
    line("}");
    return true;
  }

  // Wave mode: buffer stores and atomics (32-bit words through the buffer's
  // device address; the bound check uses the descriptor's size).
  bool emit_mubuf_write(const rdna2::Instruction& insn) {
    const std::string_view n = name_of(insn);
    const uint32_t vdata = (insn.words[1] >> 8) & 0xff;
    const bool glc = (insn.words[0] >> 14) & 1;
    std::string slot, addr, bound;
    if (!buffer_address(insn, slot, addr, bound)) return false;
    line("{");
    line(std::format("  uint64_t a = {}; bool inb = {};", addr, bound));
    int words = 0;
    if (n == "buffer_store_dword" || n == "buffer_store_format_x") words = 1;
    else if (n == "buffer_store_dwordx2" || n == "buffer_store_format_xy") words = 2;
    else if (n == "buffer_store_dwordx3" || n == "buffer_store_format_xyz") words = 3;
    else if (n == "buffer_store_dwordx4" || n == "buffer_store_format_xyzw") words = 4;
    if (words) {
      for (int k = 0; k < words; ++k) line(std::format("  if (inb) st32(a + {}ul, {});", k * 4, vreg(vdata + k)));
      line("}");
      return true;
    }
    if (n == "buffer_store_byte" || n == "buffer_store_short" || n == "buffer_store_format_d16_x") {
      const bool byte = n == "buffer_store_byte";
      line(std::format("  if (inb) {{ uint s = uint(a & uint64_t({})) * 8u; uint m = {}u << s;", byte ? 3 : 2, byte ? "0xff" : "0xffff"));
      line(std::format("    Words p = Words(gpw(a & ~uint64_t(3))); atomicAnd(p.w[0], ~m); atomicOr(p.w[0], ({} << s) & m); }}", vreg(vdata)));
      line("}");
      return true;
    }
    if (n == "buffer_store_format_d16_xy") {
      line(std::format("  if (inb) st32(a, ({} & 0xffffu) | ({} << 16));", vreg(vdata), vreg(vdata + 1)));
      line("}");
      return true;
    }
    if (n == "buffer_store_format_d16_xyzw") {
      line(std::format("  if (inb) {{ st32(a, ({} & 0xffffu) | ({} << 16)); st32(a + 4ul, ({} & 0xffffu) | ({} << 16)); }}",
                       vreg(vdata), vreg(vdata + 1), vreg(vdata + 2), vreg(vdata + 3)));
      line("}");
      return true;
    }
    static const std::pair<std::string_view, const char*> kAtomics[] = {
        {"buffer_atomic_add", "atomicAdd"}, {"buffer_atomic_umin", "atomicMin"}, {"buffer_atomic_umax", "atomicMax"},
        {"buffer_atomic_and", "atomicAnd"}, {"buffer_atomic_or", "atomicOr"}, {"buffer_atomic_xor", "atomicXor"},
        {"buffer_atomic_swap", "atomicExchange"}, {"buffer_atomic_sub", ""}, {"buffer_atomic_inc", ""},
        {"buffer_atomic_cmpswap", ""}};
    for (const auto& [k, f] : kAtomics) {
      if (n != k) continue;
      const std::string place = "Words(gpw(a & ~uint64_t(3))).w[0]";
      std::string call;
      if (n == "buffer_atomic_sub") call = std::format("atomicAdd({}, uint(-int({})))", place, vreg(vdata));
      else if (n == "buffer_atomic_inc") return fail("buffer_atomic_inc");
      else if (n == "buffer_atomic_cmpswap") call = std::format("atomicCompSwap({}, {}, {})", place, vreg(vdata + 1), vreg(vdata));
      else call = std::format("{}({}, {})", f, place, vreg(vdata));
      line(std::format("  uint r = inb ? {} : 0u;", call));
      if (glc) line(std::format("  {} = r;", vreg(vdata)));
      line("}");
      return true;
    }
    return fail("");
  }

  // MTBUF: typed loads (gfx10 unified buffer formats).
  bool emit_mtbuf(const rdna2::Instruction& insn) {
    const std::string_view n = name_of(insn);
    int comps = 0;
    if (n == "tbuffer_load_format_x") comps = 1;
    else if (n == "tbuffer_load_format_xy") comps = 2;
    else if (n == "tbuffer_load_format_xyz") comps = 3;
    else if (n == "tbuffer_load_format_xyzw") comps = 4;
    else return fail("");
    const uint32_t format = (insn.words[0] >> 19) & 0x7f;
    const uint32_t vdata = (insn.words[1] >> 8) & 0xff;
    // (bytes per element, components, kind): 'u' unorm8, 'U' unorm16, 'h' half, 'f' float, 'i' uint32
    struct Fmt {
      uint32_t id;
      int comps;
      char kind;
    };
    static const Fmt kFormats[] = {{1, 1, 'u'}, {14, 2, 'u'}, {56, 4, 'u'}, {7, 1, 'U'}, {23, 2, 'U'},
                                   {65, 4, 'U'}, {13, 1, 'h'}, {29, 2, 'h'}, {71, 4, 'h'}, {22, 1, 'f'},
                                   {64, 2, 'f'}, {74, 3, 'f'}, {77, 4, 'f'}, {20, 1, 'i'}, {62, 2, 'i'},
                                   {72, 3, 'i'}, {75, 4, 'i'}, {5, 1, 'b'}, {18, 2, 'b'}, {60, 4, 'b'}};
    const Fmt* fmt = nullptr;
    for (const Fmt& f : kFormats)
      if (f.id == format) fmt = &f;
    static const std::set<uint32_t> kFetchFormats = {1,  2,  5,  6,  7,  8,  11, 12, 13, 14, 15, 18, 19,
                                                     20, 21, 22, 23, 24, 27, 28, 29, 56, 57, 60, 61, 62,
                                                     63, 64, 65, 66, 69, 70, 71, 72, 73, 74, 75, 76, 77};
    if (wave_ && kFetchFormats.count(format)) {
      // Every gfx10 format through the typed-load helper.
      std::string d3, addr, bound;
      if (!buffer_address(insn, d3, addr, bound)) return false;
      line("{");
      line(std::format("  uvec4 t = fetch_format({}, {}u, {});", addr, format, bound));
      for (int k = 0; k < comps; ++k) line(std::format("  {} = t.{};", vreg(vdata + k), "xyzw"[k]));
      line("}");
      uses_fetch_format_ = true;
      return true;
    }
    if (!fmt) return fail(std::format("buffer format {}", format));
    std::string slot, addr, bound;
    if (!buffer_address(insn, slot, addr, bound)) return false;
    line("{");
    line(std::format("  uint64_t a = {}; bool inb = {};", addr, bound));
    std::string value[4];
    switch (fmt->kind) {
      case 'u':
        line("  uint w = inb ? ld32(a) : 0u;");
        for (int k = 0; k < 4; ++k) value[k] = std::format("floatBitsToUint(float((w >> {}u) & 0xffu) / 255.0)", k * 8);
        break;
      case 'b':
        line("  uint w = inb ? ld32(a) : 0u;");
        for (int k = 0; k < 4; ++k) value[k] = std::format("((w >> {}u) & 0xffu)", k * 8);
        break;
      case 'U':
        line("  uint w0 = inb ? ld32(a) : 0u; uint w1 = inb && " + std::to_string(fmt->comps) + " > 2 ? ld32(a + 4ul) : 0u;");
        value[0] = "floatBitsToUint(float(w0 & 0xffffu) / 65535.0)";
        value[1] = "floatBitsToUint(float(w0 >> 16) / 65535.0)";
        value[2] = "floatBitsToUint(float(w1 & 0xffffu) / 65535.0)";
        value[3] = "floatBitsToUint(float(w1 >> 16) / 65535.0)";
        break;
      case 'h':
        line("  uint w0 = inb ? ld32(a) : 0u; uint w1 = inb && " + std::to_string(fmt->comps) + " > 2 ? ld32(a + 4ul) : 0u;");
        value[0] = "floatBitsToUint(unpackHalf2x16(w0).x)";
        value[1] = "floatBitsToUint(unpackHalf2x16(w0).y)";
        value[2] = "floatBitsToUint(unpackHalf2x16(w1).x)";
        value[3] = "floatBitsToUint(unpackHalf2x16(w1).y)";
        break;
      default:  // 'f', 'i': 32-bit components
        for (int k = 0; k < 4; ++k) value[k] = std::format("(inb ? ld32(a + {}ul) : 0u)", k * 4);
        break;
    }
    const uint32_t one = fmt->kind == 'b' || fmt->kind == 'i' ? 1u : 0x3f800000u;
    for (int k = 0; k < comps; ++k) {
      const std::string v = k < fmt->comps ? value[k] : (k == 3 ? std::format("{:#x}u", one) : "0u");
      line(std::format("  {} = {};", vreg(vdata + k), v));
    }
    line("}");
    return true;
  }

  // An image's Texture / Image slot words as a uvec4 expression: the
  // CPU-resolved slot, or (gpu_desc_) a lookup of the descriptor in the SGPRs.
  std::string image_words(SlotKind kind, uint32_t srsrc) {
    if (gpu_desc_.count(index_)) {
      const std::string v = std::format("dq{}", temp_++);
      std::string w[8];
      for (int k = 0; k < 8; ++k) w[k] = sreg(srsrc + k);
      line(std::format("uvec4 {} = desc_lookup({}u, uvec4({}, {}, {}, {}), uvec4({}, {}, {}, {}));", v,
                       kind == SlotKind::Texture ? mem::kDescTexture : mem::kDescImage, w[0], w[1], w[2], w[3], w[4],
                       w[5], w[6], w[7]));
      return v;
    }
    const uint32_t slot = add_slot(kind, srsrc, 4);
    return std::format("uvec4(D({}u), D({}u), D({}u), D({}u))", slot, slot + 1, slot + 2, slot + 3);
  }

  bool emit_mimg(const rdna2::Instruction& insn) {
    const std::string_view n = name_of(insn);
    const uint32_t w0 = insn.words[0], w1 = insn.words[1];
    const uint32_t dmask = (w0 >> 8) & 0xf, nsa = (w0 >> 1) & 3;
    uint32_t dim = (w0 >> 3) & 7;
    // 1D images are 2D images of height 1 (y = 0 / the texel row's centre).
    const bool one_d = dim == 0;
    if (one_d) dim = 1;
    const uint32_t vaddr = w1 & 0xff, vdata = (w1 >> 8) & 0xff, srsrc = ((w1 >> 16) & 0x1f) * 4,
                   ssamp = ((w1 >> 21) & 0x1f) * 4;
    // Address VGPR i: consecutive from vaddr, or listed one per byte after
    // the two instruction dwords (NSA encoding).
    const auto addr_reg = [&](uint32_t i) {
      if (!nsa || i == 0) return vreg(vaddr + i);
      const uint32_t b = i - 1;
      return vreg((insn.words[2 + b / 4] >> ((b % 4) * 8)) & 0xff);
    };
    // Coordinate i (a 1D image's y is a constant: 0.5 for sampling, 0 for loads;
    // later operands such as the lod shift down by one).
    const bool sampling = starts(n, "image_sample") || starts(n, "image_gather");
    const auto A = [&](uint32_t i) {
      if (!one_d) return addr_reg(i);
      if (i == 1) return std::string(sampling ? "0x3f000000u" : "0u");
      return addr_reg(i > 1 ? i - 1 : i);
    };
    if (wave_ && (n == "image_store" || n == "image_store_mip")) {
      // UAV writes: storage images by the same dimension.
      if (dim != 1 && dim != 2 && dim != 5) return fail(std::format("image_store dimension {}", dim));
      const std::string img = image_words(SlotKind::Image, srsrc);
      std::string target, coord;
      if (dim == 1) target = std::format("images[nonuniformEXT({}.y)]", img), coord = std::format("ivec2(int({}), int({}))", A(0), A(1));
      else if (dim == 2) target = std::format("images_3d[nonuniformEXT({}.w)]", img), coord = std::format("ivec3(int({}), int({}), int({}))", A(0), A(1), A(2));
      else if (dim == 5) target = std::format("images_a[nonuniformEXT({}.z)]", img), coord = std::format("ivec3(int({}), int({}), int({}))", A(0), A(1), A(2));
      else return fail(std::format("image_store dimension {}", dim));
      std::string comps[4] = {"0.0", "0.0", "0.0", "1.0"};
      uint32_t r = vdata;
      for (int k = 0; k < 4; ++k)
        if (dmask & (1u << k)) comps[k] = std::format("uintBitsToFloat({})", vreg(r++));
      line(std::format("imageStore({}, {}, vec4({}, {}, {}, {}));", target, coord, comps[0], comps[1], comps[2], comps[3]));
      return true;
    }
    // dim: 1 2D, 2 3D, 3 cube, 5 2D array (others not yet). The texture slot
    // holds the image's index in each bindless array: 2D, 2D array, 3D. A
    // cube is a 2D texture of 6 layers: its coordinates arrive already
    // projected (v_cubesc/tc / |ma| + 1.5, face id), so it is sampled as a
    // 2D array (no filtering across faces).
    const bool cube = dim == 3 && sampling;
    if (cube) dim = 5;
    if (dim != 1 && dim != 2 && dim != 5) return fail(std::format("image dimension {}", dim));
    const std::string tex = image_words(SlotKind::Texture, srsrc);
    const char* type = dim == 1 ? "sampler2D" : dim == 2 ? "sampler3D" : "sampler2DArray";
    const std::string array =
        dim == 1 ? std::format("textures[nonuniformEXT({}.x)]", tex)
        : dim == 2 ? std::format("textures_3d[nonuniformEXT({}.w)]", tex)
                   : std::format("textures_a[nonuniformEXT({}.z)]", tex);
    const auto sampler_index = [&]() {
      if (gpu_samp_.count(index_)) return std::format("sampler_of({}, {})", sreg(ssamp), sreg(ssamp + 2));
      return std::format("D({}u)", add_slot(SlotKind::Sampler, ssamp, 1));
    };
    const int coords = dim == 1 ? 2 : 3;
    std::string value;
    if (starts(n, "image_sample") || starts(n, "image_gather4")) {
      // Name: image_sample|image_gather4 [_c] [_lz|_l|_b] [_o]. Address
      // operands in order: offset (_o), bias (_b), depth reference (_c),
      // coordinates, lod (_l).
      const bool gather = starts(n, "image_gather4");
      std::string_view rest = n.substr(gather ? 13 : 12);
      const bool has_offset = rest.ends_with("_o");
      if (has_offset) rest.remove_suffix(2);
      const bool compare = starts(rest, "_c") && !starts(rest, "_cl");
      if (compare) rest.remove_prefix(2);
      if (!rest.empty() && rest != "_lz" && rest != "_l" && rest != "_b") return fail("");
      if (dim == 2 && (gather || compare)) return fail("gather or compare on a 3D image");
      uint32_t a = 0;
      const std::string offset = has_offset ? addr_reg(a++) : "";
      const std::string bias = rest == "_b" ? addr_reg(a++) : "";
      const std::string ref = compare ? addr_reg(a++) : "";
      // Coordinate j after the prefix operands (a 1D image's y is constant;
      // later operands such as the lod shift down by one).
      const uint32_t base = a;
      const auto C = [&](uint32_t j) {
        if (!one_d) return addr_reg(base + j);
        if (j == 1) return std::string("0x3f000000u");
        return addr_reg(base + (j > 1 ? j - 1 : j));
      };
      const std::string smp = sampler_index();
      const std::string sampler = std::format("{}({}, samplers[nonuniformEXT({})])", type, array, smp);
      const std::string tsize = std::format("vec{}(textureSize({}, 0))", coords, sampler);
      line("{");
      std::string coord = std::format("uintBitsToFloat({}), uintBitsToFloat({})", C(0), C(1));
      if (coords == 3)
        coord += dim == 5 ? std::format(", roundEven(uintBitsToFloat({}))", C(2)) : std::format(", uintBitsToFloat({})", C(2));
      line(std::format("  vec{} uv = vec{}({});", coords, coords, coord));
      if (cube) line("  uv.xy -= 1.0;");
      if (has_offset) {
        // Texel offsets: signed 6-bit x, y, z in bytes 0-2.
        line(std::format("  uint o = {};", offset));
        line("  ivec3 io = ivec3(int(o << 26) >> 26, int(o << 18) >> 26, int(o << 10) >> 26);");
        if (dim == 2) line(std::format("  uv += vec3(io) / {};", tsize));
        else line(std::format("  uv.xy += vec2(io.xy) / {}.xy;", tsize));
      }
      const std::string lod = rest == "_l" ? std::format("uintBitsToFloat({})", C(coords)) : "0.0";
      if (compare) {
        // Depth compare (shadow maps) done by hand: the four texels of the
        // bilinear footprint against the reference, filtered by the sampler's
        // filter (point = nearest of the four). Compare function: S# dword 0
        // bits 12-14, result = reference OP texel.
        std::string w0;
        if (gpu_samp_.count(index_)) w0 = sreg(ssamp);
        else if (!src(ssamp, insn, w0)) return false;
        line(std::format("  vec4 g = cmp_depth(uintBitsToFloat({}), textureGather({}, uv, 0), ({} >> 12) & 7u);", ref,
                         sampler, w0));
        if (gather) {
          value = "g";
        } else {
          line(std::format("  vec2 f = fract(uv.xy * {}.xy - 0.5);", tsize));
          line(std::format("  if (({} & 1u) == 0u) f = step(vec2(0.5), f);", smp));
          value = "vec4(mix(mix(g.w, g.z, f.x), mix(g.x, g.y, f.x), f.y), 0.0, 0.0, 1.0)";
        }
      } else if (gather) {
        const int comp = dmask & 1 ? 0 : dmask & 2 ? 1 : dmask & 4 ? 2 : 3;
        value = std::format("textureGather({}, uv, {})", sampler, comp);
      } else if (rest == "_lz" || rest == "_l") {
        value = std::format("textureLod({}, uv, {})", sampler, lod);
      } else if (rest == "_b") {
        value = std::format("texture({}, uv, uintBitsToFloat({}))", sampler, bias);
      } else {
        value = std::format("texture({}, uv)", sampler);
      }
      line(std::format("  vec4 t = {};", value));
      if (gather) {
        for (int k = 0; k < 4; ++k) line(std::format("  {} = floatBitsToUint(t.{});", vreg(vdata + k), "xyzw"[k]));
      } else {
        uint32_t r = vdata;
        for (int k = 0; k < 4; ++k)
          if (dmask & (1u << k)) line(std::format("  {} = floatBitsToUint(t.{});", vreg(r++), "xyzw"[k]));
      }
      line("}");
      return true;
    } else if (n == "image_load" || n == "image_load_mip") {
      // _mip: the level follows the coordinates.
      std::string coord = std::format("int({}), int({})", A(0), A(1));
      if (coords == 3) coord += std::format(", int({})", A(2));
      const std::string lod = n == "image_load_mip" ? std::format("int({})", A(coords)) : "0";
      value = std::format("texelFetch({}({}, samplers[0]), ivec{}({}), {})", type, array, coords, coord, lod);
    } else {
      return fail("");
    }
    line("{");
    line(std::format("  vec4 t = {};", value));
    uint32_t r = vdata;
    for (int k = 0; k < 4; ++k)
      if (dmask & (1u << k)) line(std::format("  {} = floatBitsToUint(t.{});", vreg(r++), "xyzw"[k]));
    line("}");
    return true;
  }

  bool emit_exp(const rdna2::Instruction& insn) {
    const uint32_t w0 = insn.words[0], w1 = insn.words[1];
    const uint32_t en = w0 & 0xf, target = (w0 >> 4) & 0x3f, compr = (w0 >> 10) & 1;
    uint32_t src[4];
    for (int k = 0; k < 4; ++k) src[k] = (w1 >> (k * 8)) & 0xff;
    std::string comp[4];
    if (compr) {
      // Two registers of packed halves: (src0 lo, src0 hi, src1 lo, src1 hi).
      comp[0] = std::format("unpackHalf2x16({}).x", vreg(src[0]));
      comp[1] = std::format("unpackHalf2x16({}).y", vreg(src[0]));
      comp[2] = std::format("unpackHalf2x16({}).x", vreg(src[1]));
      comp[3] = std::format("unpackHalf2x16({}).y", vreg(src[1]));
    } else {
      for (int k = 0; k < 4; ++k) comp[k] = std::format("uintBitsToFloat({})", vreg(src[k]));
    }
    const auto write = [&](const std::string& var) {
      for (int k = 0; k < 4; ++k) {
        const bool on = compr ? (en & (k < 2 ? 1u : 4u)) != 0 : (en & (1u << k)) != 0;
        if (on) line(std::format("{}.{} = {};", var, "xyzw"[k], comp[k]));
      }
    };
    if (out_.stage == ShaderStage::Vertex) {
      if (target == 12) {
        write("pos");
        wrote_position_ = true;
      } else if (target >= 32 && target < 64) {
        out_.outputs = std::max(out_.outputs, target - 31);
        write(std::format("o_{}", target - 32));
      } else if (target >= 13 && target <= 15) {
        // pos1-3: point size, clip distances - not used yet
      } else {
        return fail(std::format("export target {}", target));
      }
    } else {
      if (target < 8) {
        out_.color_targets |= 1u << target;
        write(std::format("c_{}", target));
      } else if (target == 8 || target == 9) {
        // mrtz / null: depth is not exported yet
      } else {
        return fail(std::format("export target {}", target));
      }
    }
    return true;
  }

  bool emit(const rdna2::Instruction& insn) {
    if (!insn.name) return fail("unknown opcode");
    switch (insn.format) {
      case Format::Vop1: case Format::Vop2: case Format::Vop3: case Format::Vop3p: case Format::Vopc:
        return emit_valu(insn);
      case Format::Vintrp: return emit_interp(insn);
      case Format::Mubuf: return emit_mubuf(insn);
      case Format::Mtbuf: return emit_mtbuf(insn);
      case Format::Mimg: return emit_mimg(insn);
      case Format::Exp: return emit_exp(insn);
      default: return fail("");
    }
  }

  // A scalar instruction that reads a GPU-side SGPR (see v_readfirstlane_b32)
  // is evaluated in the shader; its destination becomes GPU-side too. One
  // that overwrites a GPU-side SGPR from CPU-known values makes it a CPU
  // value again.
  bool scalar_on_gpu(const rdna2::Instruction& insn) {
    const std::string_view n = name_of(insn);
    const uint32_t w0 = insn.words[0];
    uint32_t dst = 0, srcs[2] = {kNull, kNull};
    int count = 0;
    switch (insn.format) {
      case Format::Sop1: dst = (w0 >> 16) & 0x7f, srcs[0] = w0 & 0xff, count = 1; break;
      case Format::Sop2: dst = (w0 >> 16) & 0x7f, srcs[0] = w0 & 0xff, srcs[1] = (w0 >> 8) & 0xff, count = 2; break;
      case Format::Sopk: dst = (w0 >> 16) & 0x7f; srcs[0] = dst; count = n == "s_movk_i32" ? 0 : 1; break;
      case Format::Smem: {
        const uint32_t base = (w0 & 0x3f) << 1;
        if (base < 106 && (dyn_[base] || dyn_[base + 1])) return fail("memory load from a GPU-side address");
        const uint32_t offset_sgpr = insn.words[1] >> 25;
        if (offset_sgpr < 106 && dyn_[offset_sgpr] && ((w0 >> 14) & 1)) return fail("memory load at a GPU-side offset");
        dst = (w0 >> 6) & 0x7f;
        const uint32_t n_dwords = n.find("x16") != std::string_view::npos ? 16 : n.find("x8") != std::string_view::npos ? 8
                                  : n.find("x4") != std::string_view::npos ? 4 : n.find("x2") != std::string_view::npos ? 2 : 1;
        for (uint32_t k = dst; k < dst + n_dwords && k < 106; ++k) dyn_[k] = false;
        return true;
      }
      default: return true;  // SOPC/SOPP: scc and control flow stay on the CPU
    }
    bool reads_dyn = false;
    for (int k = 0; k < count; ++k)
      if (srcs[k] < 106 && dyn_[srcs[k]]) reads_dyn = true;
    if (dst >= 106) return reads_dyn ? fail("GPU-side value into a special register") : true;
    const bool wide = n.ends_with("_b64");
    if (!reads_dyn) {
      dyn_[dst] = false;
      if (wide && dst + 1 < 106) dyn_[dst + 1] = false;
      return true;
    }
    if (wide) return fail("64-bit operation on a GPU-side SGPR");
    std::string a, b;
    if (!src(srcs[0], insn, a) || (count > 1 && !src(srcs[1], insn, b))) return false;
    std::string e;
    if (n == "s_mov_b32") e = a;
    else if (n == "s_and_b32") e = std::format("{} & {}", a, b);
    else if (n == "s_or_b32") e = std::format("{} | {}", a, b);
    else if (n == "s_xor_b32") e = std::format("{} ^ {}", a, b);
    else if (n == "s_andn2_b32") e = std::format("{} & ~{}", a, b);
    else if (n == "s_add_u32" || n == "s_add_i32") e = std::format("{} + {}", a, b);
    else if (n == "s_sub_u32" || n == "s_sub_i32") e = std::format("{} - {}", a, b);
    else if (n == "s_mul_i32") e = std::format("{} * {}", a, b);
    else if (n == "s_lshl_b32") e = std::format("{} << ({} & 31u)", a, b);
    else if (n == "s_lshr_b32") e = std::format("{} >> ({} & 31u)", a, b);
    else if (n == "s_min_u32") e = std::format("min({}, {})", a, b);
    else if (n == "s_max_u32") e = std::format("max({}, {})", a, b);
    else if (n == "s_addk_i32") e = std::format("{} + {:#x}u", a, static_cast<uint32_t>(static_cast<int16_t>(w0 & 0xffff)));
    else if (n == "s_mulk_i32") e = std::format("{} * {:#x}u", a, static_cast<uint32_t>(static_cast<int16_t>(w0 & 0xffff)));
    else return fail("scalar operation on a GPU-side SGPR");
    dyn_[dst] = true;
    line(std::format("sd{} = {};", dst, e));
    return true;
  }

  void finish() {
    std::string g = "#version 460\n"
                    "#extension GL_EXT_buffer_reference : require\n"
                    "#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require\n"
                    "#extension GL_EXT_nonuniform_qualifier : require\n"
                    "layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer Words { uint w[]; };\n"
                    "%PUSH%"
                    "layout(set = 0, binding = 0) uniform texture2D textures[];\n"
                    "layout(set = 0, binding = 2) uniform texture2DArray textures_a[];\n"
                    "layout(set = 0, binding = 3) uniform texture3D textures_3d[];\n"
                    "layout(set = 0, binding = 1) uniform sampler samplers[];\n"
                    "uint D(uint i) { return Words(pc.data).w[i]; }\n"
                    "%CMP%"
                    "%MEM%"
                    "uint ld32(uint64_t a) {\n"
                    "  uint64_t al = a & ~uint64_t(3); uint sh = uint(a & uint64_t(3)) * 8u;\n"
                    "  uint w0 = Words(gpa(al)).w[0];\n"
                    "  if (sh == 0u) return w0;\n"
                    "  return (w0 >> sh) | (Words(gpa(al + 4ul)).w[0] << (32u - sh));\n"
                    "}\n";
    g.replace(g.find("%CMP%"), 5, kCmpDepth);
    g.replace(g.find("%MEM%"), 5, mem::glsl_helpers(false));
    // Push constants: the vertex stage's per-draw words at offset 0, the pixel stage's at 8, the
    // guest memory table at 16.
    const size_t push = g.find("%PUSH%");
    g.replace(push, 6,
              out_.stage == ShaderStage::Vertex
                  ? "layout(push_constant) uniform Push { uint64_t data; layout(offset = 16) uint64_t mem; } pc;\n"
                  : "layout(push_constant) uniform Push { layout(offset = 8) uint64_t data; uint64_t mem; } pc;\n");
    if (out_.stage == ShaderStage::Vertex) {
      for (uint32_t k = 0; k < out_.outputs; ++k) g += std::format("layout(location = {}) out vec4 o_{};\n", k, k);
    } else {
      for (uint32_t k = 0; k < out_.inputs; ++k)
        g += std::format("layout(location = {}) {}in vec4 a_{};\n", k, flat_inputs_.count(k) ? "flat " : "", k);
      for (uint32_t k = 0; k < 8; ++k)
        if (out_.color_targets & (1u << k)) g += std::format("layout(location = {}) out vec4 c_{};\n", k, k);
    }
    g += "void main() {\n  bool vcc = false;\n";
    const uint32_t vgprs = std::max<uint32_t>(max_vgpr_, 4);
    for (uint32_t k = 0; k < vgprs; ++k) g += std::format("  uint v{} = 0u;\n", k);
    for (uint32_t k = 0; k < 106; ++k)
      if (body_.find(std::format("sd{} ", k)) != std::string::npos || body_.find(std::format("sd{};", k)) != std::string::npos ||
          body_.find(std::format("sd{})", k)) != std::string::npos || body_.find(std::format("sd{},", k)) != std::string::npos)
        g += std::format("  uint sd{} = 0u;\n", k);
    if (out_.stage == ShaderStage::Vertex) {
      g += "  vec4 pos = vec4(0.0, 0.0, 0.0, 1.0);\n";
      g += "  v0 = uint(gl_VertexIndex); v3 = uint(gl_InstanceIndex);\n";
      for (uint32_t k = 0; k < out_.outputs; ++k) g += std::format("  o_{} = vec4(0.0);\n", k);
    } else {
      for (uint32_t k = 0; k < 8; ++k)
        if (out_.color_targets & (1u << k)) g += std::format("  c_{} = vec4(0.0);\n", k);
      // Input VGPRs after the barycentrics (v0, v1): the titles' shaders seen
      // so far enable POS_FIXED_PT next (x in the low half, y in the high half).
      if (vgprs > 2) g += "  v2 = uint(gl_FragCoord.x) | (uint(gl_FragCoord.y) << 16);\n";
    }
    g += body_;
    if (out_.stage == ShaderStage::Vertex) g += "  gl_Position = vec4(pos.x, -pos.y, pos.z, pos.w);\n";
    g += "}\n";
    out_.glsl = std::move(g);
  }

  TranslatedShader& out_;
  std::string body_;
  uint32_t index_ = 0;
  uint32_t max_vgpr_ = 0;
  bool wrote_position_ = false;
  std::set<uint32_t> flat_inputs_;
  std::array<bool, 106> dyn_{};
  const bool wave_;
  uint32_t max_sgpr_ = 0;
  std::vector<std::string> post_;  // wave mode: statements after the active-lane guard (ballots)
  std::set<uint32_t> gpu_smem_;     // wave mode: s_buffer_load instructions the GPU performs (Buffer slot)
  std::set<uint32_t> gpu_load_;     // wave mode: SMEM loads the GPU performs from the SGPR values
  std::set<uint32_t> gpu_desc_;     // wave mode: MUBUF/MTBUF/MIMG with a descriptor only the GPU knows
  std::set<uint32_t> gpu_samp_;     // wave mode: MIMG with a sampler only the GPU knows
  uint32_t temp_ = 0;               // names of GLSL temporaries
  bool uses_fetch_format_ = false;
  bool dpp_ = false;
  uint32_t lds_words_ = 0;
  uint32_t blocks_ = 1, last_leader_pc_ = 0;  // SGPRs whose value only the GPU knows (from v_readfirstlane_b32)
};

// ------------------------------------------------------------ scalar run

uint32_t sop_src(ScalarRun& run, uint32_t s, const rdna2::Instruction& insn, bool& ok) {
  uint32_t c;
  if (s < 128) return run.sgpr[s];
  if (s == kLiteral) return insn.literal;
  if (inline_constant(s, c)) return c;
  ok = false;
  run.error = std::format("scalar operand {}", s);
  return 0;
}

uint64_t sop_src64(ScalarRun& run, uint32_t s, const rdna2::Instruction& insn, bool& ok) {
  uint32_t c;
  if (s < 127) return run.sgpr[s] | (uint64_t{run.sgpr[s + 1]} << 32);
  if (s == kLiteral) return insn.literal;
  if (inline_constant(s, c)) return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(c)));
  ok = false;
  run.error = std::format("scalar operand {}", s);
  return 0;
}

bool scalar_step(const rdna2::Instruction& insn, ScalarRun& run, bool& scc, bool& stop) {
  const std::string_view n = name_of(insn);
  const uint32_t w0 = insn.words[0], w1 = insn.words[1];
  bool ok = true;
  auto& s = run.sgpr;
  switch (insn.format) {
    case Format::Sopp:
      if (n == "s_endpgm") stop = true;
      else if (n == "s_waitcnt" || n == "s_clause" || n == "s_setprio" || n == "s_inst_prefetch" || n == "s_nop" ||
               n == "s_waitcnt_depctr" || n == "s_sendmsg" || n == "s_barrier" || n == "s_ttracedata" ||
               n == "s_icache_inv" || n == "s_sleep" || n == "s_code_end")
        ;
      else return run.error = std::format("{} (branches are not supported yet)", n), false;
      return true;
    case Format::Sop1: {
      const uint32_t dst = (w0 >> 16) & 0x7f, s0 = w0 & 0xff;
      if (n == "s_mov_b32") s[dst] = sop_src(run, s0, insn, ok);
      else if (n == "s_mov_b64" || n == "s_wqm_b64") {
        const uint64_t v = sop_src64(run, s0, insn, ok);
        s[dst] = static_cast<uint32_t>(v);
        s[dst + 1] = static_cast<uint32_t>(v >> 32);
      } else if (n == "s_not_b32") s[dst] = ~sop_src(run, s0, insn, ok), scc = s[dst] != 0;
      else if (n == "s_brev_b32") {
        uint32_t v = sop_src(run, s0, insn, ok), r = 0;
        for (int k = 0; k < 32; ++k) r |= ((v >> k) & 1u) << (31 - k);
        s[dst] = r;
      } else if (n == "s_and_saveexec_b64") {
        // all lanes: exec stays all ones
        s[dst] = s[kExecLo];
        s[dst + 1] = s[kExecHi];
      } else return run.error = std::string(n), false;
      return ok;
    }
    case Format::Sopk: {
      const uint32_t dst = (w0 >> 16) & 0x7f;
      const int32_t imm = static_cast<int16_t>(w0 & 0xffff);
      if (n == "s_movk_i32") s[dst] = static_cast<uint32_t>(imm);
      else if (n == "s_addk_i32") s[dst] += static_cast<uint32_t>(imm);
      else if (n == "s_mulk_i32") s[dst] *= static_cast<uint32_t>(imm);
      else if (starts(n, "s_cmpk_")) scc = false;
      else return run.error = std::string(n), false;
      return true;
    }
    case Format::Sop2: {
      const uint32_t dst = (w0 >> 16) & 0x7f, s0 = w0 & 0xff, s1 = (w0 >> 8) & 0xff;
      if (n.ends_with("_b64")) {
        const uint64_t a = sop_src64(run, s0, insn, ok), b = sop_src64(run, s1, insn, ok);
        uint64_t r;
        if (n == "s_and_b64") r = a & b;
        else if (n == "s_or_b64") r = a | b;
        else if (n == "s_xor_b64") r = a ^ b;
        else if (n == "s_andn2_b64") r = a & ~b;
        else if (n == "s_cselect_b64") r = scc ? a : b;
        else return run.error = std::string(n), false;
        s[dst] = static_cast<uint32_t>(r);
        s[dst + 1] = static_cast<uint32_t>(r >> 32);
        scc = r != 0;
        return ok;
      }
      const uint32_t a = sop_src(run, s0, insn, ok), b = sop_src(run, s1, insn, ok);
      uint32_t r;
      if (n == "s_add_u32" || n == "s_add_i32") r = a + b, scc = (uint64_t{a} + b) >> 32;
      else if (n == "s_addc_u32") r = a + b + scc, scc = (uint64_t{a} + b + scc) >> 32;
      else if (n == "s_sub_u32" || n == "s_sub_i32") r = a - b, scc = b > a;
      else if (n == "s_mul_i32") r = a * b;
      else if (n == "s_and_b32") r = a & b, scc = r != 0;
      else if (n == "s_or_b32") r = a | b, scc = r != 0;
      else if (n == "s_xor_b32") r = a ^ b, scc = r != 0;
      else if (n == "s_andn2_b32") r = a & ~b, scc = r != 0;
      else if (n == "s_lshl_b32") r = a << (b & 31), scc = r != 0;
      else if (n == "s_lshr_b32") r = a >> (b & 31), scc = r != 0;
      else if (n == "s_ashr_i32") r = static_cast<uint32_t>(static_cast<int32_t>(a) >> (b & 31)), scc = r != 0;
      else if (n == "s_max_u32") r = std::max(a, b), scc = a > b;
      else if (n == "s_min_u32") r = std::min(a, b), scc = a < b;
      else if (n == "s_max_i32") r = static_cast<uint32_t>(std::max(static_cast<int32_t>(a), static_cast<int32_t>(b)));
      else if (n == "s_min_i32") r = static_cast<uint32_t>(std::min(static_cast<int32_t>(a), static_cast<int32_t>(b)));
      else if (n == "s_cselect_b32") r = scc ? a : b;
      else if (n == "s_bfe_u32") {
        const uint32_t off = b & 31, width = (b >> 16) & 0x7f;
        r = width >= 32 ? a >> off : (a >> off) & ((1u << width) - 1);
        scc = r != 0;
      } else if (n == "s_lshl1_add_u32") r = (a << 1) + b;
      else if (n == "s_lshl2_add_u32") r = (a << 2) + b;
      else if (n == "s_lshl3_add_u32") r = (a << 3) + b;
      else if (n == "s_lshl4_add_u32") r = (a << 4) + b;
      else if (n == "s_pack_ll_b32_b16") r = (a & 0xffff) | (b << 16);
      else if (n == "s_pack_lh_b32_b16") r = (a & 0xffff) | (b & 0xffff0000u);
      else if (n == "s_pack_hh_b32_b16") r = (a >> 16) | (b & 0xffff0000u);
      else if (n == "s_bfm_b32") r = (((a & 31) ? (1u << (a & 31)) : 0u) - 1u) << (b & 31);
      else if (n == "s_bfe_i32") {
        const uint32_t off = b & 31, width = (b >> 16) & 0x7f;
        const int32_t v = static_cast<int32_t>(a);
        r = width == 0 ? 0 : width >= 32 ? static_cast<uint32_t>(v >> off)
                        : static_cast<uint32_t>((v << (32 - off - std::min(width, 32 - off))) >> (32 - std::min(width, 32 - off)));
        scc = r != 0;
      } else if (n == "s_mul_hi_u32") r = static_cast<uint32_t>((uint64_t{a} * b) >> 32);
      else if (n == "s_mul_hi_i32") r = static_cast<uint32_t>((int64_t{static_cast<int32_t>(a)} * static_cast<int32_t>(b)) >> 32);
      else if (n == "s_absdiff_i32") r = static_cast<uint32_t>(std::abs(static_cast<int32_t>(a) - static_cast<int32_t>(b))), scc = r != 0;
      else if (n == "s_nand_b32") r = ~(a & b), scc = r != 0;
      else if (n == "s_nor_b32") r = ~(a | b), scc = r != 0;
      else if (n == "s_xnor_b32") r = ~(a ^ b), scc = r != 0;
      else if (n == "s_orn2_b32") r = a | ~b, scc = r != 0;
      else return run.error = std::string(n), false;
      s[dst] = r;
      return ok;
    }
    case Format::Sopc: {
      const uint32_t a = sop_src(run, w0 & 0xff, insn, ok), b = sop_src(run, (w0 >> 8) & 0xff, insn, ok);
      if (n == "s_cmp_eq_u32" || n == "s_cmp_eq_i32") scc = a == b;
      else if (n == "s_cmp_lg_u32" || n == "s_cmp_lg_i32") scc = a != b;
      else if (n == "s_cmp_lt_u32") scc = a < b;
      else if (n == "s_cmp_gt_u32") scc = a > b;
      else if (n == "s_cmp_ge_u32") scc = a >= b;
      else if (n == "s_cmp_le_u32") scc = a <= b;
      else if (n == "s_cmp_lt_i32") scc = static_cast<int32_t>(a) < static_cast<int32_t>(b);
      else if (n == "s_cmp_gt_i32") scc = static_cast<int32_t>(a) > static_cast<int32_t>(b);
      else if (n == "s_cmp_ge_i32") scc = static_cast<int32_t>(a) >= static_cast<int32_t>(b);
      else if (n == "s_cmp_le_i32") scc = static_cast<int32_t>(a) <= static_cast<int32_t>(b);
      else if (n == "s_bitcmp0_b32") scc = ((a >> (b & 31)) & 1) == 0;
      else if (n == "s_bitcmp1_b32") scc = ((a >> (b & 31)) & 1) != 0;
      else return run.error = std::string(n), false;
      return ok;
    }
    case Format::Smem: {
      const uint32_t sdata = (w0 >> 6) & 0x7f, sbase = (w0 & 0x3f) * 2;
      int32_t offset = static_cast<int32_t>(w1 << 11) >> 11;
      const uint32_t soffset = (w1 >> 25) & 0x7f;
      if (soffset != kNull) offset += static_cast<int32_t>(s[soffset]);
      int words = 1;
      if (n.ends_with("x2")) words = 2;
      else if (n.ends_with("x4")) words = 4;
      else if (n.ends_with("x8")) words = 8;
      else if (n.ends_with("x16")) words = 16;
      uint32_t data[16] = {};
      if (starts(n, "s_load_dword")) {
        const uint64_t base = s[sbase] | (uint64_t{s[sbase + 1] & 0xffff} << 32);
        run.read(base + static_cast<int64_t>(offset), data, words * 4u);
      } else if (starts(n, "s_buffer_load_dword")) {
        // V# in s[sbase..sbase+3]: base, stride, num_records (bytes when stride is 0)
        const uint64_t base = s[sbase] | (uint64_t{s[sbase + 1] & 0xffff} << 32);
        const uint32_t stride = (s[sbase + 1] >> 16) & 0x3fff;
        const uint64_t limit = stride ? uint64_t{s[sbase + 2]} * stride : s[sbase + 2];
        for (int k = 0; k < words; ++k) {
          const uint64_t at = static_cast<uint64_t>(offset) + k * 4u;
          if (at + 4 <= limit) run.read(base + at, &data[k], 4);
        }
      } else if (n == "s_dcache_inv" || n == "s_gl1_inv" || n == "s_memtime" || n == "s_memrealtime") {
        return true;
      } else {
        return run.error = std::string(n), false;
      }
      for (int k = 0; k < words; ++k) s[sdata + k] = data[k];
      return true;
    }
    default: return true;
  }
}

}  // namespace

ComputeInfo ComputeInfo::from_state(const uint8_t* state, size_t size) {
  // 7 dwords: ?, ?, ?, (x | y << 16), z, COMPUTE_PGM_RSRC1, COMPUTE_PGM_RSRC2
  ComputeInfo c;
  if (size < 28) return c;
  uint32_t d[7];
  std::memcpy(d, state, sizeof d);
  c.local_size[0] = std::max<uint32_t>(d[3] & 0xffff, 1);
  c.local_size[1] = std::max<uint32_t>(d[3] >> 16, 1);
  c.local_size[2] = std::max<uint32_t>(d[4] & 0xffff, 1);
  const uint32_t rsrc2 = d[6];
  c.user_sgprs = (rsrc2 >> 1) & 0x1f;
  c.tgid[0] = (rsrc2 >> 7) & 1;
  c.tgid[1] = (rsrc2 >> 8) & 1;
  c.tgid[2] = (rsrc2 >> 9) & 1;
  c.tg_size = (rsrc2 >> 10) & 1;
  c.tidig_comps = (rsrc2 >> 11) & 3;
  c.lds_bytes = ((rsrc2 >> 15) & 0x1ff) * 512;
  return c;
}

TranslatedShader translate_shader(const uint8_t* code, size_t size, ShaderStage stage, const ComputeInfo* compute) {
  TranslatedShader out;
  out.stage = stage;
  if (compute) out.compute = *compute;
  for (size_t pc = 0; pc + 4 <= size;) {
    rdna2::Instruction insn = rdna2::decode(code + pc, size - pc);
    if (insn.format == rdna2::Format::Invalid) {
      out.error = std::format("undecodable instruction at +{:#x}", pc);
      return out;
    }
    out.program.instructions.push_back({insn, static_cast<uint32_t>(pc)});
    pc += insn.size;
    if (insn.name && std::string_view(insn.name) == "s_endpgm") break;
  }
  Translator t(out);
  out.ok = t.run();
  return out;
}

bool run_scalar(const TranslatedShader& shader, ScalarRun& run, std::vector<uint32_t>& words) {
  words.assign(shader.slot_words, 0);
  if (shader.stage == ShaderStage::Compute) {
    // Wave-mode shaders: the scalar program runs once in program order
    // (branches ignored) for the values the CPU must provide - SMEM results
    // and descriptors, whose inputs the translator checked are known that
    // way. Instructions the interpreter cannot evaluate only touch values
    // the GPU computes itself.
    bool scc = false, stop = false;
    const auto& insns = shader.program.instructions;
    size_t next_slot = 0;
    std::vector<uint32_t> order(shader.slots.size());
    for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&](uint32_t x, uint32_t y) { return shader.slots[x].instruction < shader.slots[y].instruction; });
    for (uint32_t k = 0; k <= insns.size(); ++k) {
      while (next_slot < order.size() && shader.slots[order[next_slot]].instruction == k) {
        const Slot& slot = shader.slots[order[next_slot++]];
        if (slot.kind == SlotKind::Sgpr) words[slot.offset] = run.sgpr[slot.sgpr];
        else if (slot.kind != SlotKind::Blocks) run.resolve(slot.kind, &run.sgpr[slot.sgpr], &words[slot.offset]);
      }
      if (k == insns.size()) break;
      const rdna2::Instruction& insn = insns[k].insn;
      if (is_scalar(insn.format) && !is_branch(insn) && !(insn.format == Format::Sopp)) {
        stop = false;
        std::string saved = run.error;
        if (!scalar_step(insn, run, scc, stop)) run.error = saved;  // a GPU-side value: not needed here
      }
    }
    return true;
  }
  run.sgpr[kExecLo] = run.sgpr[kExecHi] = 0xffffffffu;
  bool scc = false, stop = false;
  const auto& insns = shader.program.instructions;
  // Slots by instruction (captured when that instruction is reached).
  std::vector<uint32_t> first(insns.size() + 1, static_cast<uint32_t>(shader.slots.size()));
  for (uint32_t i = static_cast<uint32_t>(shader.slots.size()); i-- > 0;) first[shader.slots[i].instruction] = i;
  for (uint32_t k = static_cast<uint32_t>(insns.size()); k-- > 0;) first[k] = std::min(first[k], first[k + 1]);
  uint32_t visited = 0;
  int64_t mask_offset = -1;
  for (const Slot& slot : shader.slots)
    if (slot.kind == SlotKind::Blocks) mask_offset = slot.offset;
  for (uint32_t k = 0, steps = 0; k < insns.size() && !stop && steps < 100000; ++steps) {
    if (k < shader.program.block_of.size()) visited |= 1u << shader.program.block_of[k];
    // Values the vector instruction at k reads, as they are before it runs.
    for (uint32_t i = first[k]; i < shader.slots.size() && shader.slots[i].instruction == k; ++i) {
      const Slot& slot = shader.slots[i];
      if (slot.kind == SlotKind::Blocks) continue;
      if (slot.kind == SlotKind::Sgpr) words[slot.offset] = run.sgpr[slot.sgpr];
      else run.resolve(slot.kind, &run.sgpr[slot.sgpr], &words[slot.offset]);
    }
    const rdna2::Instruction& insn = insns[k].insn;
    if (is_branch(insn)) {
      const std::string_view n = insn.name;
      bool taken;
      if (n == "s_branch") taken = true;
      else if (n == "s_cbranch_scc0") taken = !scc;
      else if (n == "s_cbranch_scc1") taken = scc;
      else if (n == "s_cbranch_execz") taken = false;  // all lanes are active
      else if (n == "s_cbranch_execnz") taken = true;
      else {
        run.error = std::format("+{:#x}: {} (branch on a per-lane condition)", insns[k].pc, n);
        return false;
      }
      if (taken) {
        const uint32_t target = branch_target(insns[k]);
        uint32_t t = k + 1;
        while (t < insns.size() && insns[t].pc < target) ++t;
        k = t;
        continue;
      }
      ++k;
      continue;
    }
    if (is_scalar(insn.format) && !scalar_step(insn, run, scc, stop)) {
      run.error = std::format("+{:#x}: {}", insns[k].pc, run.error);
      return false;
    }
    ++k;
  }
  if (mask_offset >= 0) words[static_cast<size_t>(mask_offset)] = visited;
  return true;
}

}  // namespace xbem::gpu
