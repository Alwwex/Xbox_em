#include <cstdint>
#include <cstring>

#include "platform/memory.h"
#include "platform/window.h"
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

// The run window converts the title's back buffer rows to 0x00RRGGBB.
TEST(window_surface_row_conversion) {
  const uint8_t rgba[8] = {0x10, 0x20, 0x30, 0xff, 0xa0, 0xb0, 0xc0, 0xff};
  uint32_t out[2] = {};
  CHECK(xbem::platform::convert_surface_row(rgba, 28, 2, 1, out));  // R8G8B8A8_UNORM
  CHECK(out[0] == 0x102030u && out[1] == 0xa0b0c0u);
  CHECK(xbem::platform::convert_surface_row(rgba, 87, 1, 2, out));  // B8G8R8A8, every 2nd pixel
  CHECK(out[0] == 0x302010u);
  const uint32_t r10[1] = {0x3ffu | (0x200u << 10) | (0u << 20)};  // R10G10B10A2: red full, green half
  CHECK(xbem::platform::convert_surface_row(reinterpret_cast<const uint8_t*>(r10), 24, 1, 1, out));
  CHECK(out[0] == 0xff8000u);
  const uint16_t half[4] = {0x3c00, 0x3800, 0x0000, 0x3c00};  // 1.0, 0.5, 0.0 (R16G16B16A16_FLOAT)
  CHECK(xbem::platform::convert_surface_row(reinterpret_cast<const uint8_t*>(half), 10, 1, 1, out));
  CHECK(out[0] == 0xff8000u);
  CHECK(!xbem::platform::convert_surface_row(rgba, 71, 1, 1, out));  // BC1: not shown
}

#if defined(__linux__)
#include <algorithm>

#include "platform/page_tracker.h"

namespace {
uint16_t written_mask(const platform::pages::Changes& c, uint64_t id) {
  for (const auto& [i, m] : c.written)
    if (i == id) return m;
  return 0;
}
}  // namespace

// Page ids, aliasing and write detection for the GPU memory mirror.
TEST(page_tracker_ids_and_writes) {
  namespace pg = platform::pages;
  const uint64_t P = pg::kPageSize;
  pg::Changes c;
  pg::collect(c);  // drop what earlier tests left
  void* anon = platform::map_guest(4 * P, P, platform::Prot::RW);
  CHECK(anon != nullptr);
  const auto a = reinterpret_cast<uint64_t>(anon);
  CHECK(a < pg::kLimit);
  CHECK_EQ(pg::page_id(a + P + 0x123), a + P);  // anonymous: the address itself

  // Two views of one physical page.
  const intptr_t mem = platform::create_shared_memory(16 * P);
  CHECK(mem >= 0);
  void* v1 = platform::map_guest(P, P, platform::Prot::RW);
  void* v2 = platform::map_guest(P, P, platform::Prot::RW);
  const auto b1 = reinterpret_cast<uint64_t>(v1), b2 = reinterpret_cast<uint64_t>(v2);
  CHECK(platform::map_shared(b1, P, platform::Prot::RW, mem, 3 * P));
  CHECK(platform::map_shared(b2, P, platform::Prot::RW, mem, 3 * P));
  const uint64_t phys = pg::kSharedId | (3 * P);
  CHECK_EQ(pg::page_id(b1), phys);
  CHECK_EQ(pg::page_id(b2 + 0x8000), phys);

  pg::watch(a + P);
  pg::watch(phys);
  pg::collect(c);
  CHECK(std::find_if(c.remapped.begin(), c.remapped.end(), [&](auto& r) { return r.first == b2; }) == c.remapped.end());
  if (pg::write_tracking()) {
    CHECK_EQ(written_mask(c, a + P), 0);  // watch() protected them: nothing written yet
    CHECK_EQ(written_mask(c, phys), 0);
  }

  static_cast<volatile uint8_t*>(anon)[P + 0x2010] = 1;  // part 2 of the watched anonymous page
  static_cast<volatile uint8_t*>(anon)[3 * P] = 1;       // not watched
  static_cast<volatile uint8_t*>(v2)[0x5000] = 7;        // through the second view: part 5
  const uint8_t bytes[4] = {1, 2, 3, 4};
  CHECK(platform::write_shared(mem, 3 * P + 0xf000, bytes, 4));  // the object itself: part 15
  pg::collect(c);
  if (pg::write_tracking()) {
    CHECK_EQ(written_mask(c, a + P), 1u << 2);
    CHECK_EQ(written_mask(c, phys), (1u << 5) | (1u << 15));
    CHECK_EQ(written_mask(c, a + 3 * P), 0);
    pg::collect(c);
    CHECK(c.written.empty());  // collected writes are re-armed
  } else {
    CHECK_EQ(written_mask(c, phys), 0xffff);
  }

  // Contents by id; GPU write-back is not a guest write.
  uint8_t page[0x10000];
  CHECK(pg::read_page(phys, page));
  CHECK(page[0x5000] == 7 && page[0xf002] == 3);
  page[0x1000] = 9;
  CHECK(pg::write_page(phys, page, 1u << 1));
  CHECK(static_cast<uint8_t*>(v1)[0x1000] == 9);
  pg::collect(c);
  if (pg::write_tracking()) CHECK_EQ(written_mask(c, phys), 0);

  // A new mapping at a watched address is reported.
  CHECK(platform::remap_anonymous(b1, P, platform::Prot::RW));
  CHECK_EQ(pg::page_id(b1), b1);
  pg::collect(c);
  CHECK(std::find_if(c.remapped.begin(), c.remapped.end(), [&](auto& r) { return r.first == b1; }) != c.remapped.end());

  pg::unwatch(a + P);
  pg::unwatch(phys);
  platform::unmap(anon, 4 * P);
  platform::unmap(v1, P);
  platform::unmap(v2, P);
  CHECK_EQ(pg::page_id(a), 0u);
}
#endif
