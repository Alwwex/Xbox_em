// xg_xs.dll: the console's texture layout library (XG).
//
// Titles ask XG how a texture is laid out in GPU memory and use its texture
// computer to copy pixel data between their linear buffers and the GPU's
// (normally tiled) layout. The emulator keeps every texture in the plain
// linear D3D12 layout - rows aligned to 256 bytes, subresources to 512 bytes,
// the same layout GetResourceAllocationInfo sizes - so:
//
//   * XGSuggestSwizzleMode answers mode 0 (SW_LINEAR);
//   * the texture computer's CopyIntoSubresource / CopyFromSubresource copy
//     rows between the caller's pitch and that layout.
//
// GetResourceLayout describes that layout in XG_RESOURCE_LAYOUT. Its fields
// were measured from what GTA VI reads: a 0x18-byte header, then up to four
// 0x6c8-byte planes - a 0x38-byte header (+0x10: bytes between array slices)
// and 15 mip levels of 0x70 bytes (+0x00 size, +0x08 offset, +0x10 2D slice
// size, +0x1c row pitch in bytes; fn 0x5121690 locks textures with them, fn
// 0x979c40 sizes staging buffers).
//
// The computer is a C++ object without QueryInterface. Its call shapes were
// measured at the title's call sites (and match those documented by the
// Leonida project): slot 0 AddRef, 1 Release, 2 GetResourceLayout(out),
// 8 CopyIntoSubresource(tiledBase, plane, subresource, linear, rowPitch,
// slicePitch), 10 CopyFromSubresource(linear, plane, subresource, tiledBase,
// rowPitch, slicePitch).

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
#include <mutex>

#include "hle/discovery.h"
#include "hle/hle_util.h"
#include "hle/probe_com.h"
#include "hle/registry.h"
#include "hle/texture_layout.h"
#include "platform/memory.h"

namespace xbem::hle {
namespace {

using namespace win;

constexpr uint64_t kRowAlign = 256, kSubresourceAlign = 512;
uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

struct ComputerState {
  uint8_t desc[128] = {};
  std::mutex mutex;
  // Linear layout, learnt from the copies: subresource -> (offset, size).
  // Titles copy subresources in order, so subresource n starts where n-1 ends.
  // Used only when the description is not a texture the layout can be
  // computed for.
  std::map<uint32_t, std::pair<uint64_t, uint64_t>> layout;

  bool computed() const {
    uint32_t dimension;
    uint64_t width;
    std::memcpy(&dimension, desc, 4);
    std::memcpy(&width, desc + 16, 8);
    return dimension >= 1 && dimension <= 4 && width > 0;
  }

  // Offset of `subresource` and its row pitch in the GPU layout.
  std::pair<uint64_t, uint64_t> offset_of(uint32_t subresource, uint64_t row_pitch, uint64_t slice_pitch) {
    if (computed()) {
      const TextureLayout l = linear_layout(desc);
      const uint32_t mip = subresource % l.mips, slice = (subresource / l.mips) % l.slices;
      return {slice * l.slice_stride + l.mip[mip].offset, l.mip[mip].pitch_bytes};
    }
    std::lock_guard lock(mutex);
    const uint64_t rows = row_pitch ? slice_pitch / row_pitch : 0;
    const uint64_t size = align_up(align_up(row_pitch, kRowAlign) * rows, kSubresourceAlign);
    if (auto it = layout.find(subresource); it != layout.end()) return {it->second.first, align_up(row_pitch, kRowAlign)};
    uint64_t offset = 0;
    if (subresource > 0 && !layout.empty()) {
      auto before = layout.lower_bound(subresource);
      if (before != layout.begin()) {
        --before;
        offset = before->second.first + before->second.second;
      }
    }
    layout[subresource] = {offset, size};
    return {offset, align_up(row_pitch, kRowAlign)};
  }
};

std::atomic<int> g_logged{0};
bool log_now() { return g_logged.fetch_add(1) < 12; }

ComputerState* state_of(ProbeObject* self) { return static_cast<ComputerState*>(self->state); }

uint32_t XBEM_GUEST_ABI computer_add_ref(ProbeObject* self) { return self->refs.fetch_add(1) + 1; }
uint32_t XBEM_GUEST_ABI computer_release(ProbeObject* self) { return self->refs.fetch_sub(1) - 1; }

// 2 GetResourceLayout(XG_RESOURCE_LAYOUT*)
constexpr size_t kLayoutHeader = 0x18, kPlaneSize = 0x6c8, kPlaneHeader = 0x38, kMipSize = 0x70, kMaxMips = 15,
                 kMaxPlanes = 4;

template <class T>
void put(uint8_t* p, size_t offset, T value) {
  std::memcpy(p + offset, &value, sizeof value);
}

HRESULT XBEM_GUEST_ABI computer_get_layout(ProbeObject* self, uint8_t* out) {
  if (!out) return E_POINTER;
  ComputerState* state = state_of(self);
  if (!state->computed()) {
    probe_log(self, 2, std::format("GetResourceLayout({}) - not a texture, not filled", static_cast<void*>(out)),
              XBEM_RETURN_ADDRESS());
    return E_INVALIDARG;
  }
  const TextureLayout l = linear_layout(state->desc);
  std::memset(out, 0, kLayoutHeader + kMaxPlanes * kPlaneSize);
  put<uint64_t>(out, 0x00, l.total);
  put<uint64_t>(out, 0x08, 0x10000);  // 64 KiB, as placed resources are aligned
  put<uint32_t>(out, 0x10, l.mips);
  put<uint32_t>(out, 0x14, 1);  // planes
  uint8_t* plane = out + kLayoutHeader;
  put<uint64_t>(plane, 0x08, l.total);
  put<uint64_t>(plane, 0x10, l.slice_stride);
  put<uint64_t>(plane, 0x18, 0x10000);
  put<uint32_t>(plane, 0x20, l.element.bytes);
  for (uint32_t m = 0; m < l.mips && m < kMaxMips; ++m) {
    const MipLayout& ml = l.mip[m];
    uint8_t* mip = plane + kPlaneHeader + m * kMipSize;
    put<uint64_t>(mip, 0x00, ml.size);
    put<uint64_t>(mip, 0x08, ml.offset);
    put<uint64_t>(mip, 0x10, ml.slice_2d);
    put<uint32_t>(mip, 0x18, ml.pitch_elements);
    put<uint32_t>(mip, 0x1c, ml.pitch_bytes);
    put<uint32_t>(mip, 0x20, 512);  // alignment
    put<uint32_t>(mip, 0x24, ml.pitch_elements);
    put<uint32_t>(mip, 0x28, ml.height_elements);
    put<uint32_t>(mip, 0x2c, l.dimension == 4 ? ml.depth : l.slices);
    put<uint32_t>(mip, 0x30, ml.width_elements);
    put<uint32_t>(mip, 0x34, ml.height_elements);
    put<uint32_t>(mip, 0x38, l.dimension == 4 ? ml.depth : l.slices);
    put<uint32_t>(mip, 0x3c, l.samples);
  }
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 8)
    XBEM_INFO("xg", "GetResourceLayout({}) from {} -> {} bytes, {} mips, mip 0 pitch {}", static_cast<void*>(out),
              describe_code_address(XBEM_RETURN_ADDRESS()), l.total, l.mips,
              l.mip.empty() ? 0 : l.mip[0].pitch_bytes);
  return S_OK;
}

void copy_rows(uint8_t* dst, uint64_t dst_pitch, const uint8_t* src, uint64_t src_pitch, uint64_t row_bytes,
               uint64_t rows) {
  for (uint64_t r = 0; r < rows; ++r) std::memcpy(dst + r * dst_pitch, src + r * src_pitch, row_bytes);
}

// 8 CopyIntoSubresource(tiledBase, plane, subresource, linear, rowPitch, slicePitch)
HRESULT XBEM_GUEST_ABI computer_copy_into(ProbeObject* self, uint8_t* tiled, uint32_t plane, uint32_t subresource,
                                          const uint8_t* linear, uint32_t row_pitch, uint32_t slice_pitch) {
  const auto [offset, gpu_pitch] = state_of(self)->offset_of(subresource, row_pitch, slice_pitch);
  if (log_now())
    XBEM_INFO("xg", "CopyIntoSubresource(base {}, plane {}, subresource {}, linear {}, pitch {} / {}) -> offset {:#x}",
              static_cast<void*>(tiled), plane, subresource, static_cast<const void*>(linear), row_pitch,
              slice_pitch, offset);
  if (!tiled || !linear || !row_pitch) return E_INVALIDARG;
  copy_rows(tiled + offset, gpu_pitch, linear, row_pitch, std::min<uint64_t>(row_pitch, gpu_pitch),
            slice_pitch / row_pitch);
  return S_OK;
}

// 10 CopyFromSubresource(linear, plane, subresource, tiledBase, rowPitch, slicePitch)
HRESULT XBEM_GUEST_ABI computer_copy_from(ProbeObject* self, uint8_t* linear, uint32_t plane, uint32_t subresource,
                                          const uint8_t* tiled, uint32_t row_pitch, uint32_t slice_pitch) {
  const auto [offset, gpu_pitch] = state_of(self)->offset_of(subresource, row_pitch, slice_pitch);
  if (log_now())
    XBEM_INFO("xg", "CopyFromSubresource(linear {}, plane {}, subresource {}, base {}, pitch {} / {}) <- offset {:#x}",
              static_cast<void*>(linear), plane, subresource, static_cast<const void*>(tiled), row_pitch,
              slice_pitch, offset);
  if (!tiled || !linear || !row_pitch) return E_INVALIDARG;
  copy_rows(linear, row_pitch, tiled + offset, gpu_pitch, std::min<uint64_t>(row_pitch, gpu_pitch),
            slice_pitch / row_pitch);
  return S_OK;
}

// GetResourceTiling(numTiles*, D3D12_PACKED_MIP_INFO*, D3D12_TILE_SHAPE*,
// numSubresourceTilings* (in/out), firstSubresource, D3D12_SUBRESOURCE_TILING*)
// with D3D12's standard 64 KiB tiles: tile shapes of 256x256 (1-byte
// elements) down to 64x64 (16-byte elements, BC blocks); a mip with a side
// smaller than the tile and all smaller ones are "packed" into whole tiles of
// their own. GTA VI sizes its streamed textures from this (fn 0x1403441f0);
// an empty answer gave them no pages while their release freed the full size.
HRESULT computer_get_tiling(ProbeObject* self, uint32_t* num_tiles, uint8_t* packed, uint32_t* shape,
                            uint32_t* num_subresources, uint32_t first, uint8_t* tilings) {
  ComputerState* state = state_of(self);
  if (!state->computed()) return E_INVALIDARG;
  const TextureLayout l = linear_layout(state->desc);
  if (l.mip.empty()) return E_INVALIDARG;
  const uint32_t e = std::max<uint32_t>(l.element.bytes, 1);
  const uint32_t tile_w = e <= 2 ? 256 : e <= 8 ? 128 : 64;
  const uint32_t tile_h = e == 1 ? 256 : e == 2 ? 128 : e == 4 ? 128 : 64;
  uint32_t standard = 0;
  while (standard < l.mips && l.mip[standard].width_elements >= tile_w && l.mip[standard].height_elements >= tile_h)
    ++standard;
  uint64_t packed_bytes = 0;
  for (uint32_t m = standard; m < l.mips; ++m) packed_bytes += uint64_t{l.mip[m].width_elements} * l.mip[m].height_elements * e * std::max<uint32_t>(l.mip[m].depth, 1);
  const uint32_t packed_tiles = standard < l.mips ? static_cast<uint32_t>((packed_bytes + 0xFFFF) / 0x10000) : 0;
  std::vector<uint32_t> start(l.mips), wide(l.mips), high(l.mips), deep(l.mips);
  uint32_t per_slice = 0;
  for (uint32_t m = 0; m < standard; ++m) {
    wide[m] = (l.mip[m].width_elements + tile_w - 1) / tile_w;
    high[m] = (l.mip[m].height_elements + tile_h - 1) / tile_h;
    deep[m] = std::max<uint32_t>(l.mip[m].depth, 1);
    start[m] = per_slice;
    per_slice += wide[m] * high[m] * deep[m];
  }
  const uint32_t packed_start = per_slice;
  per_slice += packed_tiles;
  const uint32_t slices = std::max<uint32_t>(l.slices, 1);
  if (num_tiles) *num_tiles = per_slice * slices;
  if (packed) {
    packed[0] = static_cast<uint8_t>(standard);
    packed[1] = static_cast<uint8_t>(l.mips - standard);
    put<uint32_t>(packed, 4, packed_tiles);
    put<uint32_t>(packed, 8, packed_tiles ? packed_start : 0);
  }
  if (shape) {
    const uint32_t texels = l.element.block ? 4 : 1;
    shape[0] = tile_w * texels;
    shape[1] = tile_h * texels;
    shape[2] = 1;
  }
  if (num_subresources && tilings) {
    const uint32_t total = l.mips * slices;
    uint32_t n = first < total ? std::min(*num_subresources, total - first) : 0;
    for (uint32_t k = 0; k < n; ++k) {
      const uint32_t sub = first + k, m = sub % l.mips, slice = sub / l.mips;
      uint8_t* t = tilings + k * 12;
      const bool is_packed = m >= standard;
      put<uint32_t>(t, 0, is_packed ? 0 : wide[m]);
      put<uint16_t>(t, 4, static_cast<uint16_t>(is_packed ? 0 : high[m]));
      put<uint16_t>(t, 6, static_cast<uint16_t>(is_packed ? 0 : deep[m]));
      put<uint32_t>(t, 8, is_packed ? 0xFFFFFFFFu : slice * per_slice + start[m]);
    }
    *num_subresources = n;
  }
  static const bool log_all = std::getenv("XBEM_XG_LOG") != nullptr;
  if (log_all) {
    std::string mips;
    for (uint32_t m = 0; m < standard; ++m) mips += std::format(" {}x{}@{}", wide[m], high[m], start[m]);
    XBEM_INFO("xg", "tiling {}x{}x{} dim {} format {} mips {} slices {} -> {} tiles/slice, std{} packed {} in {} @{}",
              l.mip[0].width, l.mip[0].height, l.mip[0].depth, l.dimension, l.format, l.mips, slices, per_slice,
              mips, l.mips - standard, packed_tiles, packed_start);
  } else if (log_now())
    XBEM_INFO("xg", "GetResourceTiling({}x{} format {}, {} mips) -> {} tiles, {} standard mips, {} packed in {} tiles",
              l.mip[0].width, l.mip[0].height, l.format, l.mips, per_slice * slices, standard, l.mips - standard,
              packed_tiles);
  return S_OK;
}

// Slot 10 is called in two shapes: GetResourceTiling (fn 0x1403441f0: the
// second argument is a pointer) and CopyFromSubresource (a plane number).
HRESULT XBEM_GUEST_ABI computer_slot10(ProbeObject* self, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                                       uint64_t a5, uint64_t a6) {
  if (a2 >= 0x10000)
    return computer_get_tiling(self, reinterpret_cast<uint32_t*>(a1), reinterpret_cast<uint8_t*>(a2),
                               reinterpret_cast<uint32_t*>(a3), reinterpret_cast<uint32_t*>(a4),
                               static_cast<uint32_t>(a5), reinterpret_cast<uint8_t*>(a6));
  return computer_copy_from(self, reinterpret_cast<uint8_t*>(a1), static_cast<uint32_t>(a2), static_cast<uint32_t>(a3),
                            reinterpret_cast<const uint8_t*>(a4), static_cast<uint32_t>(a5), static_cast<uint32_t>(a6));
}

const ProbeVtable& computer_vtable() {
  static const ProbeVtable v = make_probe_vtable({{0, reinterpret_cast<void*>(&computer_add_ref)},
                                                  {1, reinterpret_cast<void*>(&computer_release)},
                                                  {2, reinterpret_cast<void*>(&computer_get_layout)},
                                                  {8, reinterpret_cast<void*>(&computer_copy_into)},
                                                  {10, reinterpret_cast<void*>(&computer_slot10)}});
  return v;
}

std::string hex_words(const uint8_t* p, size_t n) {
  std::string out;
  for (size_t i = 0; i + 8 <= n; i += 8) {
    uint64_t v;
    std::memcpy(&v, p + i, 8);
    out += std::format(" {:016x}", v);
  }
  return out;
}

// HRESULT XGCreateTextureComputer(const XG_RESOURCE_DESC*, XGTextureAddressComputer** out)
HRESULT XBEM_GUEST_ABI hle_XGCreateTextureComputer(const uint8_t* desc, void** out) {
  if (!out) return E_POINTER;
  auto* state = new ComputerState;
  if (desc) platform::safe_read(reinterpret_cast<uint64_t>(desc), state->desc, sizeof state->desc);
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 8)
    XBEM_INFO("xg", "XGCreateTextureComputer(desc:{}) from {} -> linear-layout computer", hex_words(state->desc, 64),
              describe_code_address(XBEM_RETURN_ADDRESS()));
  *out = make_probe_object("xg.texture_computer", &computer_vtable(), state);
  return S_OK;
}

// HRESULT XGSuggestSwizzleMode(const desc*, reserved, uint32_t* mode)
HRESULT XBEM_GUEST_ABI hle_XGSuggestSwizzleMode(const void*, uint64_t, uint32_t* mode) {
  if (mode && reinterpret_cast<uint64_t>(mode) >= 0x10000) {
    const uint32_t linear = 0;  // SW_LINEAR: textures stay in the linear D3D12 layout
    platform::safe_write(reinterpret_cast<uint64_t>(mode), &linear, 4);
  }
  return S_OK;
}

}  // namespace

ModuleDef make_xg_module() {
  ModuleDef m;
  m.name = "xg_xs.dll";
  m.description = "XG texture layout library: linear layouts, subresource copies";
  m.exports = {XBEM_HLE(XGCreateTextureComputer), XBEM_HLE(XGSuggestSwizzleMode)};
  return m;
}

}  // namespace xbem::hle
