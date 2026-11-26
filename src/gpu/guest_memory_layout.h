#pragma once

// How translated shaders reach guest memory: the GPU keeps copies of the guest
// pages it uses (gpu/vk/guest_memory.h) and one buffer of 32-bit words - the
// "table" - says where each guest page's copy is:
//
//   words [0, 512)            device address of each pool chunk (lo, hi)
//   words [512, 512 + 16384)  directory: guest address >> 26 -> leaf (0 = none)
//   leaf k at kLeafWord + k * 1024: (address >> 16) & 1023 -> pool page
//   kWrittenWord..            16 bits per pool page: 4 KiB parts the GPU wrote
//   kMissWord..               count, then guest pages (address >> 16) shaders
//                             reached without a copy (the renderer copies
//                             them in after the submission: demand paging
//                             with a delay of one submission)
//   kMissedWord..             1 bit per guest page: already in the miss list
//
// Pool page n lives at chunk[n >> 12] + (n & 0xfff) * 64 KiB; page 0 is a
// scratch page that every unmapped address reaches (reads garbage, writes
// are dropped). Addresses with bit 63 set are device addresses already (data
// the renderer put in its upload ring) and are used as they are.

#include <cstdint>
#include <string>

namespace xbem::gpu::mem {

constexpr uint32_t kChunkPages = 4096;  // 256 MiB per chunk
constexpr uint32_t kMaxChunks = 256;
constexpr uint32_t kDirWord = 512, kDirEntries = 16384;  // covers addresses below 2^40
constexpr uint32_t kLeafWord = kDirWord + kDirEntries;
constexpr uint32_t kLeafEntries = 1024, kMaxLeaves = 4096;
constexpr uint32_t kWrittenWord = kLeafWord + kMaxLeaves * kLeafEntries;
constexpr uint32_t kMissWord = kWrittenWord + kMaxChunks * kChunkPages / 2;
constexpr uint32_t kMaxMisses = 16384;
constexpr uint32_t kMissedWord = kMissWord + 1 + kMaxMisses;
constexpr uint32_t kTableWords = kMissedWord + (1u << 24) / 32;
constexpr uint64_t kDirect = 1ull << 63;

// GLSL: gpa(va) translates a guest address for reading; with `writes` also
// gpw(va) for writing (marks the part written). Both report pages without a
// copy in the miss list. Needs `pc.mem` (uint64_t: the table's device
// address).
inline std::string glsl_helpers(bool writes) {
  const auto n = [](uint32_t v) { return std::to_string(v) + "u"; };
  return "layout(buffer_reference, std430, buffer_reference_align = 4) buffer MemT { uint w[]; };\n"
         "uint64_t mem_page(uint64_t va, out uint idx) {\n"
         "  MemT t = MemT(pc.mem);\n"
         "  idx = 0u;\n"
         "  if (va < (uint64_t(1) << 40)) {\n"
         "    uint leaf = t.w[" + n(kDirWord) + " + uint(va >> 26)];\n"
         "    if (leaf != 0u) idx = t.w[" + n(kLeafWord) + " + leaf * 1024u + (uint(va >> 16) & 1023u)];\n"
         "    if (idx == 0u) {\n"
         "      uint pg = uint(va >> 16), bit = 1u << (pg & 31u), at = " + n(kMissedWord) + " + (pg >> 5);\n"
         "      if ((t.w[at] & bit) == 0u && (atomicOr(t.w[at], bit) & bit) == 0u) {\n"
         "        uint k = atomicAdd(t.w[" + n(kMissWord) + "], 1u);\n"
         "        if (k < " + n(kMaxMisses) + ") t.w[" + n(kMissWord) + " + 1u + k] = pg;\n"
         "      }\n"
         "    }\n"
         "  }\n"
         "  uint c = (idx >> 12) * 2u;\n"
         "  return packUint2x32(uvec2(t.w[c], t.w[c + 1u])) + (uint64_t(idx & 0xfffu) << 16) + (va & uint64_t(0xffff));\n"
         "}\n"
         "uint64_t gpa(uint64_t va) {\n"
         "  if ((va >> 63) != uint64_t(0)) return va & ~(uint64_t(1) << 63);\n"
         "  uint idx; return mem_page(va, idx);\n"
         "}\n" + std::string(!writes ? "" :
         "uint64_t gpw(uint64_t va) {\n"
         "  if ((va >> 63) != uint64_t(0)) return va & ~(uint64_t(1) << 63);\n"
         "  uint idx; uint64_t a = mem_page(va, idx);\n"
         "  if (idx != 0u) {\n"
         "    uint bit = 1u << ((idx & 1u) * 16u + (uint(va >> 12) & 15u));\n"
         "    MemT t = MemT(pc.mem);\n"
         "    uint at = " + n(kWrittenWord) + " + (idx >> 1);\n"
         "    if ((t.w[at] & bit) == 0u) atomicOr(t.w[at], bit);\n"
         "  }\n"
         "  return a;\n"
         "}\n");
}

}  // namespace xbem::gpu::mem
