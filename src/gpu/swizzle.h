#pragma once

// RDNA2 (gfx10.3) texture swizzling. Console titles stream textures in the
// GPU's swizzled layouts (256B/4KB/64KB blocks in Z, S, D or R order; the
// mode is in the console part of the resource description). The renderer
// reads them through a table of element offsets computed with AMD's addrlib
// (third_party/addrlib).

#include <cstdint>
#include <memory>
#include <vector>

namespace xbem::gpu {

struct SwizzleTable {
  uint32_t width = 0, height = 0;  // mip 0, in elements (4x4 blocks for BC formats)
  uint32_t bytes = 0;              // bytes per element
  std::vector<uint64_t> offset;    // byte offset of each element of mip 0 (row-major), slice/depth 0
  uint64_t slice_size = 0;         // bytes between array slices (or depth slices of a 3D texture)
  uint64_t size = 0;               // whole surface
};

// Mip 0's element offsets for a texture of `width` x `height` elements of
// `bytes` bytes, `mips` mip levels and `slices` array slices (or depth when
// `volume`) in swizzle mode `mode` (ADDR_SW_*; 0 = linear). Cached; null when
// addrlib does not accept the description.
std::shared_ptr<const SwizzleTable> swizzle_table(uint32_t mode, uint32_t bytes, uint32_t width, uint32_t height,
                                                  uint32_t slices, bool volume, uint32_t mips);

// The 64 KiB-tile layout of a tiled (reserved) texture as the console lays
// it out (addrlib, 64KB_S): per mip its first tile and size in tiles within
// one slice; mips from `first_tail` on share the mip tail. Unlike D3D12's
// standard layout the tail comes first and mip 0 last.
struct TileLayout {
  uint32_t block_width = 0, block_height = 0, block_depth = 1;  // elements per 64 KiB tile
  uint32_t mips = 0, first_tail = 0;  // first_tail == mips: no tail
  struct Mip {
    uint32_t first_tile = 0, wide = 0, high = 0, deep = 1;
  } mip[16];
  uint32_t tail_tile = 0, tail_tiles = 0;
  uint32_t tiles_per_slice = 0;
};
// False when addrlib does not accept the description.
bool tile_layout(uint32_t bytes, uint32_t width, uint32_t height, uint32_t depth, bool volume, uint32_t mips,
                 TileLayout& out);

}  // namespace xbem::gpu
