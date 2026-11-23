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
// The computer is a C++ object without QueryInterface. Its call shapes were
// measured at the title's call sites (and match those documented by the
// Leonida project): slot 0 AddRef, 1 Release, 2 GetResourceLayout(out),
// 8 CopyIntoSubresource(tiledBase, plane, subresource, linear, rowPitch,
// slicePitch), 10 CopyFromSubresource(linear, plane, subresource, tiledBase,
// rowPitch, slicePitch).

#include <atomic>
#include <cstring>
#include <format>
#include <map>
#include <mutex>

#include "hle/discovery.h"
#include "hle/hle_util.h"
#include "hle/probe_com.h"
#include "hle/registry.h"
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
  std::map<uint32_t, std::pair<uint64_t, uint64_t>> layout;

  uint64_t offset_of(uint32_t subresource, uint64_t row_pitch, uint64_t slice_pitch) {
    std::lock_guard lock(mutex);
    const uint64_t rows = row_pitch ? slice_pitch / row_pitch : 0;
    const uint64_t size = align_up(align_up(row_pitch, kRowAlign) * rows, kSubresourceAlign);
    if (auto it = layout.find(subresource); it != layout.end()) return it->second.first;
    uint64_t offset = 0;
    if (subresource > 0 && !layout.empty()) {
      auto before = layout.lower_bound(subresource);
      if (before != layout.begin()) {
        --before;
        offset = before->second.first + before->second.second;
      }
    }
    layout[subresource] = {offset, size};
    return offset;
  }
};

std::atomic<int> g_logged{0};
bool log_now() { return g_logged.fetch_add(1) < 12; }

ComputerState* state_of(ProbeObject* self) { return static_cast<ComputerState*>(self->state); }

uint32_t XBEM_GUEST_ABI computer_add_ref(ProbeObject* self) { return self->refs.fetch_add(1) + 1; }
uint32_t XBEM_GUEST_ABI computer_release(ProbeObject* self) { return self->refs.fetch_sub(1) - 1; }

// 2 GetResourceLayout(XG_RESOURCE_LAYOUT*): the structure is not known, so
// it is left as the caller prepared it.
HRESULT XBEM_GUEST_ABI computer_get_layout(ProbeObject* self, void* out) {
  probe_log(self, 2, std::format("GetResourceLayout({}) - structure unknown, not filled", out), XBEM_RETURN_ADDRESS());
  return S_OK;
}

void copy_rows(uint8_t* dst, uint64_t dst_pitch, const uint8_t* src, uint64_t src_pitch, uint64_t row_bytes,
               uint64_t rows) {
  for (uint64_t r = 0; r < rows; ++r) std::memcpy(dst + r * dst_pitch, src + r * src_pitch, row_bytes);
}

// 8 CopyIntoSubresource(tiledBase, plane, subresource, linear, rowPitch, slicePitch)
HRESULT XBEM_GUEST_ABI computer_copy_into(ProbeObject* self, uint8_t* tiled, uint32_t plane, uint32_t subresource,
                                          const uint8_t* linear, uint32_t row_pitch, uint32_t slice_pitch) {
  const uint64_t offset = state_of(self)->offset_of(subresource, row_pitch, slice_pitch);
  if (log_now())
    XBEM_INFO("xg", "CopyIntoSubresource(base {}, plane {}, subresource {}, linear {}, pitch {} / {}) -> offset {:#x}",
              static_cast<void*>(tiled), plane, subresource, static_cast<const void*>(linear), row_pitch,
              slice_pitch, offset);
  if (!tiled || !linear || !row_pitch) return E_INVALIDARG;
  copy_rows(tiled + offset, align_up(row_pitch, kRowAlign), linear, row_pitch, row_pitch, slice_pitch / row_pitch);
  return S_OK;
}

// 10 CopyFromSubresource(linear, plane, subresource, tiledBase, rowPitch, slicePitch)
HRESULT XBEM_GUEST_ABI computer_copy_from(ProbeObject* self, uint8_t* linear, uint32_t plane, uint32_t subresource,
                                          const uint8_t* tiled, uint32_t row_pitch, uint32_t slice_pitch) {
  const uint64_t offset = state_of(self)->offset_of(subresource, row_pitch, slice_pitch);
  if (log_now())
    XBEM_INFO("xg", "CopyFromSubresource(linear {}, plane {}, subresource {}, base {}, pitch {} / {}) <- offset {:#x}",
              static_cast<void*>(linear), plane, subresource, static_cast<const void*>(tiled), row_pitch,
              slice_pitch, offset);
  if (!tiled || !linear || !row_pitch) return E_INVALIDARG;
  copy_rows(linear, row_pitch, tiled + offset, align_up(row_pitch, kRowAlign), row_pitch, slice_pitch / row_pitch);
  return S_OK;
}

const ProbeVtable& computer_vtable() {
  static const ProbeVtable v = make_probe_vtable({{0, reinterpret_cast<void*>(&computer_add_ref)},
                                                  {1, reinterpret_cast<void*>(&computer_release)},
                                                  {2, reinterpret_cast<void*>(&computer_get_layout)},
                                                  {8, reinterpret_cast<void*>(&computer_copy_into)},
                                                  {10, reinterpret_cast<void*>(&computer_copy_from)}});
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
