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

}  // namespace xbem::gpu
