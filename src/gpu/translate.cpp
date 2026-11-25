#include "gpu/translate.h"

#include <array>

#include <algorithm>
#include <cstring>
#include <format>
#include <map>
#include <set>
#include <string_view>

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

// ------------------------------------------------------------ translation

class Translator {
 public:
  Translator(TranslatedShader& out) : out_(out) {}

  bool run() {
    body_.reserve(8192);
    for (uint32_t k = 0; k < out_.program.instructions.size(); ++k) {
      index_ = k;
      const rdna2::Instruction& insn = out_.program.instructions[k].insn;
      if (is_scalar(insn.format)) {
        if (name_of(insn) == "s_endpgm") break;
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
    finish();
    return true;
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

  // A source operand as a uint expression.
  bool src(uint32_t s, const rdna2::Instruction& insn, std::string& e) {
    uint32_t c;
    if (s == kSdwaSrc0 || s == kSdwaSrc1) return e = std::format("sdwa{}_{}", s - kSdwaSrc0, index_), true;
    if (s >= 256) return e = vreg(s - 256), true;
    if (s == kLiteral) return e = std::format("{:#x}u", insn.literal), true;
    if (inline_constant(s, c)) return e = std::format("{:#x}u", c), true;
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
    const bool is_float = n.find("_f32") != std::string_view::npos || n.find("_f16") != std::string_view::npos;
    for (int k = 0; k < (insn.format == Format::Vop1 ? 1 : 2); ++k) {
      if (is_float && v.sel[k] != 6) return fail("SDWA sub-dword float operand");
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
      if (v.sdst != kVccLo) return fail("SDWA compare into an SGPR pair");
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
        if (insn.format == Format::Vop3) {
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
    // compares into vcc
    if (starts(n, "v_cmp_")) {
      static const std::pair<std::string_view, const char*> kOps[] = {
          {"eq", "=="}, {"lg", "!="}, {"neq", "!="}, {"ne", "!="}, {"lt", "<"},
          {"le", "<="}, {"gt", ">"}, {"ge", ">="}, {"nlt", ">="}, {"nle", ">"},
          {"ngt", "<="}, {"nge", "<"}};
      const std::string_view rest = n.substr(6);
      const size_t us = rest.find('_');
      if (us == std::string_view::npos) return fail("");
      const std::string_view op = rest.substr(0, us), type = rest.substr(us + 1);
      const char* cmp = nullptr;
      for (const auto& [k, o] : kOps)
        if (op == k) cmp = o;
      if (!cmp) return fail("");
      if (insn.format == Format::Vop3 && v.sdst != kVccLo) return fail("compare into an SGPR pair");
      if (type == "f32") {
        if (!fsrc(v, 0, insn, a) || !fsrc(v, 1, insn, b)) return false;
      } else if (type == "u32" || type == "i32") {
        if (!src(v.src[0], insn, a) || !src(v.src[1], insn, b)) return false;
        if (type == "i32") {
          a = std::format("int({})", a);
          b = std::format("int({})", b);
        }
      } else {
        return fail("");
      }
      line(std::format("vcc = {} {} {};", a, cmp, b));
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
  bool buffer_address(const rdna2::Instruction& insn, std::string& slot, std::string& addr, std::string& bound) {
    const uint32_t w0 = insn.words[0], w1 = insn.words[1];
    const uint32_t offset = w0 & 0xfff, offen = (w0 >> 12) & 1, idxen = (w0 >> 13) & 1;
    const uint32_t vaddr = w1 & 0xff, srsrc = ((w1 >> 16) & 0x1f) * 4, soffset = (w1 >> 24) & 0xff;
    const uint32_t base = add_slot(SlotKind::Buffer, srsrc, 4);
    slot = std::to_string(base);
    std::string so;
    if (!src(soffset, insn, so)) return false;
    std::string index = idxen ? vreg(vaddr) : "0u";
    std::string voff = offen ? vreg(vaddr + idxen) : "0u";
    addr = std::format("(packUint2x32(uvec2(D({0}), D({0} + 1u))) + uint64_t({1}) * uint64_t(D({0} + 2u)) + "
                       "uint64_t({2} + {3} + {4}u))",
                       base, index, voff, so, offset);
    bound = idxen ? std::format("({} < D({} + 3u))", index, base)
                  : std::format("({} + {} + {}u < D({} + 3u))", voff, so, offset, base);
    return true;
  }

  bool emit_mubuf(const rdna2::Instruction& insn) {
    const std::string_view n = name_of(insn);
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

  bool emit_mimg(const rdna2::Instruction& insn) {
    const std::string_view n = name_of(insn);
    const uint32_t w0 = insn.words[0], w1 = insn.words[1];
    const uint32_t dmask = (w0 >> 8) & 0xf, dim = (w0 >> 3) & 7, nsa = (w0 >> 1) & 3;
    const uint32_t vaddr = w1 & 0xff, vdata = (w1 >> 8) & 0xff, srsrc = ((w1 >> 16) & 0x1f) * 4,
                   ssamp = ((w1 >> 21) & 0x1f) * 4;
    if (nsa) return fail("NSA image addresses");
    // dim: 1 2D, 2 3D, 5 2D array (others not yet). The texture slot holds
    // the image's index in each bindless array: 2D, 2D array, 3D.
    if (dim != 1 && dim != 2 && dim != 5) return fail(std::format("image dimension {}", dim));
    const uint32_t tex = add_slot(SlotKind::Texture, srsrc, 4);
    const char* type = dim == 1 ? "sampler2D" : dim == 2 ? "sampler3D" : "sampler2DArray";
    const std::string array =
        dim == 1 ? std::format("textures[nonuniformEXT(D({}))]", tex)
        : dim == 2 ? std::format("textures_3d[nonuniformEXT(D({}))]", tex + 3)
                   : std::format("textures_a[nonuniformEXT(D({}))]", tex + 2);
    const int coords = dim == 1 ? 2 : 3;
    std::string value;
    if (n == "image_sample" || n == "image_sample_lz" || n == "image_sample_l" || n == "image_sample_b") {
      const uint32_t smp = add_slot(SlotKind::Sampler, ssamp, 1);
      // image_sample_b takes the bias before the coordinates, _l the lod after.
      const uint32_t c = vaddr + (n == "image_sample_b" ? 1 : 0);
      std::string coord = std::format("uintBitsToFloat({}), uintBitsToFloat({})", vreg(c), vreg(c + 1));
      if (coords == 3)
        coord += dim == 5 ? std::format(", roundEven(uintBitsToFloat({}))", vreg(c + 2))
                          : std::format(", uintBitsToFloat({})", vreg(c + 2));
      coord = std::format("vec{}({})", coords, coord);
      const std::string sampler = std::format("{}({}, samplers[nonuniformEXT(D({}))])", type, array, smp);
      if (n == "image_sample_lz") value = std::format("textureLod({}, {}, 0.0)", sampler, coord);
      else if (n == "image_sample_l") value = std::format("textureLod({}, {}, uintBitsToFloat({}))", sampler, coord, vreg(c + coords));
      else value = std::format("texture({}, {})", sampler, coord);
    } else if (n == "image_load") {
      std::string coord = std::format("int({}), int({})", vreg(vaddr), vreg(vaddr + 1));
      if (coords == 3) coord += std::format(", int({})", vreg(vaddr + 2));
      value = std::format("texelFetch({}({}, samplers[0]), ivec{}({}), 0)", type, array, coords, coord);
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
                    "uint ld32(uint64_t a) {\n"
                    "  uint64_t al = a & ~uint64_t(3); uint sh = uint(a & uint64_t(3)) * 8u;\n"
                    "  uint w0 = Words(al).w[0];\n"
                    "  if (sh == 0u) return w0;\n"
                    "  return (w0 >> sh) | (Words(al).w[1] << (32u - sh));\n"
                    "}\n";
    // Push constants: the vertex stage's per-draw words at offset 0, the pixel stage's at 8.
    const size_t push = g.find("%PUSH%");
    g.replace(push, 6,
              out_.stage == ShaderStage::Vertex
                  ? "layout(push_constant) uniform Push { uint64_t data; } pc;\n"
                  : "layout(push_constant) uniform Push { layout(offset = 8) uint64_t data; } pc;\n");
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
  std::array<bool, 106> dyn_{};  // SGPRs whose value only the GPU knows (from v_readfirstlane_b32)
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

TranslatedShader translate_shader(const uint8_t* code, size_t size, ShaderStage stage) {
  TranslatedShader out;
  out.stage = stage;
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
  run.sgpr[kExecLo] = run.sgpr[kExecHi] = 0xffffffffu;
  bool scc = false, stop = false;
  size_t next_slot = 0;
  const auto& insns = shader.program.instructions;
  for (uint32_t k = 0; k < insns.size() && !stop; ++k) {
    // Values the vector instruction at k reads, as they are before it runs.
    for (; next_slot < shader.slots.size() && shader.slots[next_slot].instruction == k; ++next_slot) {
      const Slot& slot = shader.slots[next_slot];
      if (slot.kind == SlotKind::Sgpr) words[slot.offset] = run.sgpr[slot.sgpr];
      else run.resolve(slot.kind, &run.sgpr[slot.sgpr], &words[slot.offset]);
    }
    if (is_scalar(insns[k].insn.format) && !scalar_step(insns[k].insn, run, scc, stop)) {
      run.error = std::format("+{:#x}: {}", insns[k].pc, run.error);
      return false;
    }
  }
  return true;
}

}  // namespace xbem::gpu
