#pragma once

// The memory layout the emulator gives textures: D3D12's linear layout - every
// mip of every array slice in order, rows aligned to 256 bytes and
// subresources to 512. D3D12 (GetResourceAllocationInfo, placed resources)
// and XG (the texture computer's layouts and copies) share it, so a title's
// own address arithmetic agrees with the copies the emulator makes.

#include <cstdint>
#include <vector>

namespace xbem::hle {

// DXGI format -> element size. Block-compressed formats are 4x4 blocks of
// `bytes`; others are one pixel of `bytes`.
struct FormatInfo {
  uint32_t bytes;  // per pixel, or per 4x4 block when block is true
  bool block;
};
FormatInfo format_info(uint32_t dxgi_format);

struct MipLayout {
  uint64_t offset = 0;    // within an array slice
  uint64_t size = 0;      // 512-aligned
  uint64_t slice_2d = 0;  // one depth slice: pitch x rows
  uint32_t pitch_bytes = 0;
  uint32_t pitch_elements = 0;
  uint32_t width = 0, height = 0, depth = 0;  // in pixels
  uint32_t width_elements = 0, height_elements = 0;  // in elements (blocks)
};

struct TextureLayout {
  uint32_t dimension = 0;  // D3D12_RESOURCE_DIMENSION
  uint32_t format = 0;
  uint32_t mips = 0, slices = 0, samples = 1;
  FormatInfo element{};
  std::vector<MipLayout> mip;
  uint64_t slice_stride = 0;  // bytes of one array slice (all its mips)
  uint64_t total = 0;
};

// Layout of a D3D12_RESOURCE_DESC (Dimension, Alignment, Width, Height,
// DepthOrArraySize, MipLevels, Format, SampleDesc, ...; XG_RESOURCE_DESC
// starts the same way). Buffers get one "mip" of Width bytes.
TextureLayout linear_layout(const uint8_t* desc);

}  // namespace xbem::hle
