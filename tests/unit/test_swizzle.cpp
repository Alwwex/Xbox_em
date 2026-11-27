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

TEST(tile_layout_tail_first_mip0_last) {
  // BC7 512x512 (128x128 elements of 16 bytes, 64x64 per 64 KiB tile), 10 mips:
  // mip 0 is 2x2 tiles, mip 1 one tile, mips 2-9 share the tail.
  TileLayout l;
  CHECK(tile_layout(16, 128, 128, 1, false, 10, l));
  CHECK_EQ(l.block_width, 64u);
  CHECK_EQ(l.block_height, 64u);
  CHECK_EQ(l.first_tail, 2u);
  CHECK_EQ(l.mip[0].wide, 2u);
  CHECK_EQ(l.mip[0].high, 2u);
  CHECK_EQ(l.mip[1].wide, 1u);
  CHECK_EQ(l.tail_tile, 0u);
  CHECK_EQ(l.mip[1].first_tile, 1u);
  CHECK_EQ(l.mip[0].first_tile, 2u);
  CHECK_EQ(l.tiles_per_slice, 6u);
}
