#include "hle/texture_layout.h"

#include <algorithm>
#include <cstring>

namespace xbem::hle {

FormatInfo format_info(uint32_t f) {
  if (f >= 1 && f <= 4) return {16, false};    // R32G32B32A32
  if (f >= 5 && f <= 8) return {12, false};    // R32G32B32
  if (f >= 9 && f <= 22) return {8, false};    // R16G16B16A16, R32G32, R32G8X24
  if (f >= 23 && f <= 47) return {4, false};   // R10G10B10A2 .. R24G8
  if (f >= 48 && f <= 59) return {2, false};   // R8G8, R16
  if (f >= 60 && f <= 66) return {1, false};   // R8, A8, R1 (rounded up)
  if (f >= 67 && f <= 69) return {4, false};   // R9G9B9E5, R8G8_B8G8, G8R8_G8B8
  if (f >= 70 && f <= 72) return {8, true};    // BC1
  if (f >= 73 && f <= 78) return {16, true};   // BC2, BC3
  if (f >= 79 && f <= 81) return {8, true};    // BC4
  if (f >= 82 && f <= 84) return {16, true};   // BC5
  if (f == 85 || f == 86 || f == 115) return {2, false};  // B5G6R5, B5G5R5A1, B4G4R4A4
  if (f >= 87 && f <= 93) return {4, false};   // B8G8R8A8 / X8
  if (f >= 94 && f <= 99) return {16, true};   // BC6H, BC7
  if (f == 100 || f == 101 || f == 107) return {4, false};  // AYUV, Y410, YUY2
  if (f == 102 || f == 108 || f == 109) return {8, false};  // Y416, Y210, Y216
  if (f >= 103 && f <= 106) return {2, false};  // NV12/P010/P016/420_OPAQUE (upper bound)
  if (f >= 110 && f <= 114) return {2, false};
  return {4, false};  // unknown / console-specific: assume 32 bits per pixel
}

TextureLayout linear_layout(const uint8_t* desc) {
  TextureLayout l;
  uint32_t height, samples;
  uint64_t width;
  uint16_t depth_or_array, mips;
  std::memcpy(&l.dimension, desc, 4);
  std::memcpy(&width, desc + 16, 8);
  std::memcpy(&height, desc + 24, 4);
  std::memcpy(&depth_or_array, desc + 28, 2);
  std::memcpy(&mips, desc + 30, 2);
  std::memcpy(&l.format, desc + 32, 4);
  std::memcpy(&samples, desc + 36, 4);
  l.samples = std::clamp<uint32_t>(samples, 1, 16);
  if (l.dimension == 1) {  // BUFFER
    l.mips = l.slices = 1;
    l.element = {1, false};
    MipLayout m;
    m.size = m.slice_2d = width;
    m.width = m.width_elements = static_cast<uint32_t>(std::min<uint64_t>(width, UINT32_MAX));
    m.height = m.depth = m.height_elements = 1;
    m.pitch_bytes = m.pitch_elements = m.width;
    l.mip.push_back(m);
    l.slice_stride = l.total = width;
    return l;
  }
  width = std::max<uint64_t>(width, 1);
  height = std::max<uint32_t>(height, 1);
  const uint32_t depth = l.dimension == 4 ? std::max<uint16_t>(depth_or_array, 1) : 1;
  l.slices = l.dimension == 4 ? 1 : std::max<uint16_t>(depth_or_array, 1);
  if (mips == 0) {  // full chain
    uint64_t largest = std::max<uint64_t>({width, height, depth});
    while (largest) {
      ++mips;
      largest >>= 1;
    }
  }
  l.mips = mips;
  l.element = format_info(l.format);
  const FormatInfo& fi = l.element;
  uint64_t offset = 0;
  for (uint32_t i = 0; i < mips; ++i) {
    MipLayout m;
    m.width = static_cast<uint32_t>(std::max<uint64_t>(width >> i, 1));
    m.height = std::max<uint32_t>(height >> i, 1);
    m.depth = std::max<uint32_t>(depth >> i, 1);
    m.width_elements = fi.block ? (m.width + 3) / 4 : m.width;
    m.height_elements = fi.block ? (m.height + 3) / 4 : m.height;
    const uint64_t row = uint64_t{m.width_elements} * fi.bytes;
    m.pitch_bytes = static_cast<uint32_t>((row + 255) & ~255ull);
    m.pitch_elements = m.pitch_bytes / fi.bytes;
    m.slice_2d = uint64_t{m.pitch_bytes} * m.height_elements;
    m.size = (m.slice_2d * m.depth + 511) & ~511ull;
    m.offset = offset;
    offset += m.size;
    l.mip.push_back(m);
  }
  l.slice_stride = offset;
  l.total = std::min<uint64_t>(offset * l.slices * l.samples, 16ull << 30);
  return l;
}

}  // namespace xbem::hle
