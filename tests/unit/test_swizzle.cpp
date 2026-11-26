// RDNA2 swizzle tables from addrlib.

#include <set>

#include "gpu/swizzle.h"
#include "test.h"

using namespace xbem::gpu;

TEST(swizzle_64kb_s_is_a_permutation) {
  const auto t = swizzle_table(9 /*64KB_S*/, 4, 256, 256, 1, false, 1);
  CHECK(t != nullptr);
  CHECK_EQ(t->offset.size(), 256u * 256u);
  CHECK_EQ(t->offset[0], 0u);
  CHECK_EQ(t->offset[1], 4u);  // x bit 0 is the lowest element bit
  std::set<uint64_t> seen(t->offset.begin(), t->offset.end());
  CHECK_EQ(seen.size(), t->offset.size());
  CHECK(*seen.rbegin() < t->size);
}

TEST(swizzle_linear_is_row_major) {
  const auto t = swizzle_table(0 /*LINEAR*/, 4, 64, 4, 1, false, 1);
  CHECK(t != nullptr);
  CHECK_EQ(t->offset[1], 4u);
  CHECK(t->offset[64] >= 256u);  // next row after a pitch of at least 64 elements
}

TEST(swizzle_4kb_z_is_morton_order) {
  // 16-byte elements: a 4 KiB block holds 16x16 of them in Z (Morton) order
  const auto t = swizzle_table(4 /*4KB_Z*/, 16, 16, 16, 1, false, 1);
  CHECK(t != nullptr);
  CHECK_EQ(t->offset[1], 16u);           // (1, 0)
  CHECK_EQ(t->offset[16], 32u);          // (0, 1)
  CHECK_EQ(t->offset[16 + 1], 48u);      // (1, 1)
  CHECK_EQ(t->offset[2], 64u);           // (2, 0)
  std::set<uint64_t> seen(t->offset.begin(), t->offset.end());
  CHECK_EQ(seen.size(), t->offset.size());
}
