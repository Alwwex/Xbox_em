// RDNA2 -> GLSL translator: instruction forms GTA VI's UI shaders use.
// Programs are hand-encoded (ISA encodings, no title code).

#include <cstring>
#include <string>
#include <vector>

#include "gpu/translate.h"
#include "test.h"

using namespace xbem::gpu;

namespace {

TranslatedShader translate(const std::vector<uint32_t>& words, ShaderStage stage) {
  std::vector<uint8_t> code(words.size() * 4);
  std::memcpy(code.data(), words.data(), code.size());
  return translate_shader(code.data(), code.size(), stage);
}

bool has(const TranslatedShader& t, const std::string& text) { return t.glsl.find(text) != std::string::npos; }

constexpr uint32_t kEndpgm = 0xbf810000;

}  // namespace

TEST(translate_fmaak_and_fmamk) {
  // v_fmaak_f32 v1, v2, v3, 2.0 ; v_fmamk_f32 v4, v5, 0.5, v6
  const TranslatedShader t = translate({0x5a020702, 0x40000000, 0x58080d05, 0x3f000000, kEndpgm}, ShaderStage::Pixel);
  CHECK(t.ok);
  CHECK(has(t, "v1 = floatBitsToUint(fma(uintBitsToFloat(v2), uintBitsToFloat(v3), uintBitsToFloat(0x40000000u)))"));
  CHECK(has(t, "v4 = floatBitsToUint(fma(uintBitsToFloat(v5), uintBitsToFloat(0x3f000000u), uintBitsToFloat(v6)))"));
}

TEST(translate_sdwa_word_select) {
  // v_and_b32_sdwa v8, 63, v2 src1_sel:WORD1 (src0 is an inline constant)
  const TranslatedShader t = translate({0x361004f9, 0x058606bf, kEndpgm}, ShaderStage::Pixel);
  CHECK(t.ok);
  CHECK(has(t, "bitfieldExtract(v2, 16, 16)"));
  CHECK(has(t, "v8 = sdwa0_0 & sdwa1_0;"));
  // the pixel position input in v2
  CHECK(has(t, "v2 = uint(gl_FragCoord.x) | (uint(gl_FragCoord.y) << 16);"));
}

TEST(translate_readfirstlane_makes_a_gpu_side_sgpr) {
  // v_readfirstlane_b32 s16, v11 ; s_and_b32 s16, s16, 31 ; v_mov_b32 v10, s16
  const TranslatedShader t = translate({0x7e20050b, 0x87109f10, 0x7e140210, kEndpgm}, ShaderStage::Pixel);
  CHECK(t.ok);
  CHECK(has(t, "sd16 = v11;"));
  CHECK(has(t, "sd16 = sd16 & 0x1fu;"));
  CHECK(has(t, "v10 = sd16;"));
  CHECK(has(t, "uint sd16 = 0u;"));
  CHECK(t.slots.empty());  // nothing comes from the CPU
}

TEST(translate_uniform_branch_gates_blocks) {
  // s_cmp_eq_u32 s0, 1 ; s_cbranch_scc0 +1 ; v_mov_b32 v1, 1.0 ; v_mov_b32 v2, 2.0 ; s_endpgm
  const TranslatedShader t =
      translate({0xbf068100, 0xbf840001, 0x7e0202f2, 0x7e0402f4, kEndpgm}, ShaderStage::Pixel);
  CHECK(t.ok);
  CHECK(has(t, "if ((D(0) & 0x2u) != 0u) {"));  // v1 = 1.0 sits in block 1
  for (uint32_t s0 : {1u, 0u}) {
    ScalarRun run;
    run.sgpr[0] = s0;
    run.read = [](uint64_t, void*, size_t) { return false; };
    run.resolve = [](SlotKind, const uint32_t*, uint32_t*) {};
    std::vector<uint32_t> words;
    CHECK(run_scalar(t, run, words));
    // s0 == 1: scc set, no branch - blocks 0, 1, 2; otherwise block 1 is skipped
    CHECK_EQ(words[0], s0 == 1 ? 0x7u : 0x5u);
  }
}

TEST(scalar_run_bitfield_mask) {
  // s_bfm_b32 s2, 4, 8 ; v_mov_b32 v0, s2 ; s_endpgm (mask of 4 bits at bit 8)
  const TranslatedShader t = translate({0x92028884, 0x7e000202, kEndpgm}, ShaderStage::Pixel);
  CHECK(t.ok);
  ScalarRun run;
  run.read = [](uint64_t, void*, size_t) { return false; };
  run.resolve = [](SlotKind, const uint32_t*, uint32_t*) {};
  std::vector<uint32_t> words;
  CHECK(run_scalar(t, run, words));
  CHECK_EQ(words[0], 0xf00u);
}
