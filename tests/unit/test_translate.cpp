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
