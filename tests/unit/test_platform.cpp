#include <cstdint>
#include <cstring>

#include "platform/memory.h"
#include "test.h"

using namespace xbem;

TEST(safe_read_valid_and_invalid) {
  const char text[] = "guid-or-string";
  char out[sizeof text] = {};
  CHECK(platform::safe_read(reinterpret_cast<uint64_t>(text), out, sizeof text));
  CHECK(std::memcmp(out, text, sizeof text) == 0);
  CHECK(!platform::safe_read(0x10, out, 8));                     // unmapped low page
  void* p = platform::map_anywhere(platform::page_size(), platform::page_size(), platform::Prot::None);
  CHECK(p != nullptr);
  CHECK(!platform::safe_read(reinterpret_cast<uint64_t>(p), out, 8));  // mapped but inaccessible
  platform::unmap(p, platform::page_size());
}

#if defined(__x86_64__) && !defined(_WIN32)
#include "platform/crash.h"

// CLWB / CLFLUSHOPT (Zen 2 has them; older host CPUs fault) become NOPs of the
// same length, keeping the addressing bytes.
TEST(patch_cache_hint_instructions) {
  void* page = platform::map_anywhere(platform::page_size(), platform::page_size(), platform::Prot::RW);
  CHECK(page != nullptr);
  auto* code = static_cast<uint8_t*>(page);
  const uint8_t clwb_r9[] = {0x66, 0x41, 0x0f, 0xae, 0x31};               // clwb [r9]
  const uint8_t clflushopt_disp[] = {0x66, 0x0f, 0xae, 0xb8, 0x40, 0, 0, 0};  // clflushopt [rax+0x40]
  const uint8_t clflush[] = {0x0f, 0xae, 0x38};                          // clflush [rax]: supported everywhere
  std::memcpy(code, clwb_r9, sizeof clwb_r9);
  CHECK(std::strcmp(platform::patch_cache_hint_instruction(code), "CLWB") == 0);
  const uint8_t nop_r9[] = {0x66, 0x41, 0x0f, 0x1f, 0x01};
  CHECK(std::memcmp(code, nop_r9, sizeof nop_r9) == 0);
  CHECK(platform::patch_cache_hint_instruction(code) == nullptr);  // already patched
  std::memcpy(code, clflushopt_disp, sizeof clflushopt_disp);
  CHECK(std::strcmp(platform::patch_cache_hint_instruction(code), "CLFLUSHOPT") == 0);
  CHECK(code[2] == 0x1f && code[3] == 0x80 && code[4] == 0x40);
  std::memcpy(code, clflush, sizeof clflush);
  CHECK(platform::patch_cache_hint_instruction(code) == nullptr);
  platform::unmap(page, platform::page_size());
}
#endif
