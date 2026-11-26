#include "gpu/swizzle.h"

#include <cstdlib>
#include <algorithm>
#include <bit>
#include <cstring>
#include <map>
#include <mutex>
#include <tuple>

#include "addrinterface.h"

namespace xbem::gpu {
namespace {

void* ADDR_API alloc_sys(const ADDR_ALLOCSYSMEM_INPUT* in) { return std::malloc(in->sizeInBytes); }
ADDR_E_RETURNCODE ADDR_API free_sys(const ADDR_FREESYSMEM_INPUT* in) {
  std::free(in->pVirtAddr);
  return ADDR_OK;
}

// One addrlib instance for an RDNA2 GPU (Navi 21: family 143, revision 0x28).
// The pipe/bank configuration only matters for the XOR modes.
ADDR_HANDLE library() {
  static ADDR_HANDLE handle = [] {
    ADDR_CREATE_INPUT in{};
    ADDR_CREATE_OUTPUT out{};
    in.size = sizeof in;
    out.size = sizeof out;
    in.chipEngine = 0xD;    // CIASICIDGFXENGINE_ARCTICISLAND (gfx9 and later)
    in.chipFamily = 0x8F;   // FAMILY_NV
    in.chipRevision = 0x28; // Navi 21
    in.callbacks.allocSysMem = alloc_sys;
    in.callbacks.freeSysMem = free_sys;
    in.regValue.gbAddrConfig = 0x00000444;
    return AddrCreate(&in, &out) == ADDR_OK ? out.hLib : nullptr;
  }();
  return handle;
}

using Key = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, bool, uint32_t>;
std::mutex g_mutex;
std::map<Key, std::shared_ptr<const SwizzleTable>> g_tables;

}  // namespace

std::shared_ptr<const SwizzleTable> swizzle_table(uint32_t mode, uint32_t bytes, uint32_t width, uint32_t height,
                                                  uint32_t slices, bool volume, uint32_t mips) {
  const Key key{mode, bytes, width, height, slices, volume, mips};
  {
    std::lock_guard lock(g_mutex);
    if (auto it = g_tables.find(key); it != g_tables.end()) return it->second;
  }
  std::shared_ptr<SwizzleTable> t;
  ADDR_HANDLE lib = library();
  // Z order (4KB_Z = 4, 64KB_Z = 8): addrlib refuses these for gfx10
  // textures. The surface is laid out like the S mode of the same block size;
  // inside a block the elements follow a Morton curve (x bit first).
  const bool z_order = mode == 4 || mode == 8;
  const uint32_t layout_mode = z_order ? mode + 1 : mode;
  const uint64_t elements = uint64_t{width} * height;
  if (lib && bytes && width && height && elements <= (16u << 20)) {
    ADDR2_COMPUTE_SURFACE_INFO_INPUT si{};
    ADDR2_COMPUTE_SURFACE_INFO_OUTPUT so{};
    si.size = sizeof si;
    so.size = sizeof so;
    si.flags.texture = 1;
    si.swizzleMode = static_cast<AddrSwizzleMode>(layout_mode);
    si.resourceType = volume ? ADDR_RSRC_TEX_3D : ADDR_RSRC_TEX_2D;
    si.bpp = bytes * 8;
    si.width = width;
    si.height = height;
    si.numSlices = std::max(slices, 1u);
    si.numMipLevels = std::max(mips, 1u);
    si.numSamples = si.numFrags = 1;
    if (Addr2ComputeSurfaceInfo(lib, &si, &so) == ADDR_OK) {
      t = std::make_shared<SwizzleTable>();
      t->width = width;
      t->height = height;
      t->bytes = bytes;
      t->slice_size = so.sliceSize;
      t->size = so.surfSize;
      t->offset.resize(elements);
      ADDR2_COMPUTE_SURFACE_ADDRFROMCOORD_INPUT ai{};
      ADDR2_COMPUTE_SURFACE_ADDRFROMCOORD_OUTPUT ao{};
      ai.size = sizeof ai;
      ao.size = sizeof ao;
      ai.swizzleMode = si.swizzleMode;
      ai.flags = si.flags;
      ai.resourceType = si.resourceType;
      ai.bpp = si.bpp;
      ai.unalignedWidth = width;
      ai.unalignedHeight = height;
      ai.numSlices = si.numSlices;
      ai.numMipLevels = si.numMipLevels;
      ai.numSamples = ai.numFrags = 1;
      ai.pitchInElement = so.pitch;
      if (z_order) {
        // mip 0 starts where element (0, 0) of the S layout is
        ai.x = ai.y = 0;
        if (Addr2ComputeSurfaceAddrFromCoord(lib, &ai, &ao) != ADDR_OK) t.reset();
        const uint64_t base = t ? ao.addr : 0;
        const uint32_t block_bytes = mode == 4 ? 0x1000 : 0x10000;
        const uint32_t per_block = block_bytes / bytes, log_n = std::countr_zero(per_block);
        const uint32_t bw = 1u << ((log_n + 1) / 2), bh = 1u << (log_n / 2);
        const uint32_t blocks_per_row = (so.pitch + bw - 1) / bw;
        for (uint32_t y = 0; y < height && t; ++y)
          for (uint32_t x = 0; x < width; ++x) {
            const uint32_t bx = x % bw, by = y % bh;
            uint64_t morton = 0;
            for (uint32_t b = 0, out = 0; out < log_n; ++b) {
              if ((1u << b) < bw && out < log_n) morton |= uint64_t{(bx >> b) & 1} << out++;
              if ((1u << b) < bh && out < log_n) morton |= uint64_t{(by >> b) & 1} << out++;
            }
            const uint64_t block = uint64_t{y / bh} * blocks_per_row + x / bw;
            t->offset[uint64_t{y} * width + x] = base + block * block_bytes + morton * bytes;
          }
      }
      for (uint32_t y = 0; y < height && t && !z_order; ++y)
        for (uint32_t x = 0; x < width; ++x) {
          ai.x = x;
          ai.y = y;
          if (Addr2ComputeSurfaceAddrFromCoord(lib, &ai, &ao) != ADDR_OK) {
            t.reset();
            break;
          }
          t->offset[uint64_t{y} * width + x] = ao.addr;
        }
    }
  }
  std::lock_guard lock(g_mutex);
  g_tables[key] = t;
  return t;
}

}  // namespace xbem::gpu
