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
//   kDescWord..               image descriptors shaders read on the GPU ->
//                             bindless indices: a hash table of 16-word
//                             entries (kind, the 8 descriptor words, the 4
//                             words a CPU-resolved slot would hold)
//   kDescMissWord..           count, then descriptors not in the table (kind
//                             + 8 words; the renderer adds them after the
//                             submission), kDescMissedWord.. 1 bit per hash
//                             so a descriptor is listed once
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
constexpr uint32_t kDescWord = kMissedWord + (1u << 24) / 32;
constexpr uint32_t kDescEntries = 16384, kDescEntryWords = 16;
constexpr uint32_t kDescMissWord = kDescWord + kDescEntries * kDescEntryWords;
constexpr uint32_t kMaxDescMisses = 1024, kDescMissWords = 9;
constexpr uint32_t kDescMissedWord = kDescMissWord + 1 + kMaxDescMisses * kDescMissWords;
constexpr uint32_t kDescMissedBits = 1u << 16;
// A compute shader whose loop guard ended it writes its tag here (see the
// translator's block loop); the renderer reports and clears it.
constexpr uint32_t kGuardWord = kDescMissedWord + kDescMissedBits / 32;
// The block a loop guard stopped in (next to its tag).
constexpr uint32_t kGuardBlockWord = kGuardWord + 1;
constexpr uint32_t kTableWords = kGuardWord + 2;
// Descriptor kinds in the table (as gpu::SlotKind's Texture / Image).
constexpr uint32_t kDescTexture = 1, kDescImage = 2;

// The table's hash of a descriptor (the same in GLSL below).
inline uint32_t desc_hash(uint32_t kind, const uint32_t* w) {
  uint32_t h = kind * 0x9e3779b9u;
  for (int i = 0; i < 8; ++i) h = (h ^ w[i]) * 0x01000193u;
  return h ^ (h >> 16);
}
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
         "uint desc_hash(uint kind, uvec4 a, uvec4 b) {\n"
         "  uint h = kind * 0x9e3779b9u;\n"
         "  h = (h ^ a.x) * 0x01000193u; h = (h ^ a.y) * 0x01000193u; h = (h ^ a.z) * 0x01000193u; h = (h ^ a.w) * 0x01000193u;\n"
         "  h = (h ^ b.x) * 0x01000193u; h = (h ^ b.y) * 0x01000193u; h = (h ^ b.z) * 0x01000193u; h = (h ^ b.w) * 0x01000193u;\n"
         "  return h ^ (h >> 16);\n"
         "}\n"
         // An image descriptor's bindless indices (what its Texture / Image
         // slot would hold); unknown ones are reported and read as index 0.
         "uvec4 desc_lookup(uint kind, uvec4 a, uvec4 b) {\n"
         "  MemT t = MemT(pc.mem);\n"
         "  uint h = desc_hash(kind, a, b);\n"
         "  for (uint i = 0u; i < 8u; ++i) {\n"
         "    uint e = " + n(kDescWord) + " + ((h + i) & " + n(kDescEntries - 1) + ") * " + n(kDescEntryWords) + ";\n"
         "    uint tag = t.w[e];\n"
         "    if (tag == 0u) break;\n"
         "    if (tag == kind && t.w[e + 1u] == a.x && t.w[e + 2u] == a.y && t.w[e + 3u] == a.z && t.w[e + 4u] == a.w &&\n"
         "        t.w[e + 5u] == b.x && t.w[e + 6u] == b.y && t.w[e + 7u] == b.z && t.w[e + 8u] == b.w)\n"
         "      return uvec4(t.w[e + 9u], t.w[e + 10u], t.w[e + 11u], t.w[e + 12u]);\n"
         "  }\n"
         "  uint bit = 1u << (h & 31u), at = " + n(kDescMissedWord) + " + ((h >> 5) & " + n(kDescMissedBits / 32 - 1) + ");\n"
         "  if ((t.w[at] & bit) == 0u && (atomicOr(t.w[at], bit) & bit) == 0u) {\n"
         "    uint k = atomicAdd(t.w[" + n(kDescMissWord) + "], 1u);\n"
         "    if (k < " + n(kMaxDescMisses) + ") {\n"
         "      uint m = " + n(kDescMissWord + 1) + " + k * " + n(kDescMissWords) + ";\n"
         "      t.w[m] = kind; t.w[m + 1u] = a.x; t.w[m + 2u] = a.y; t.w[m + 3u] = a.z; t.w[m + 4u] = a.w;\n"
         "      t.w[m + 5u] = b.x; t.w[m + 6u] = b.y; t.w[m + 7u] = b.z; t.w[m + 8u] = b.w;\n"
         "    }\n"
         "  }\n"
         "  return uvec4(0u, 0x00010001u, 0u, 0u);\n"
         "}\n"
         // A sampler slot's word from an S# (see the renderer's resolve()).
         "uint sampler_of(uint w0, uint w2) {\n"
         "  uint clamp = w0 & 7u;\n"
         "  return (((w2 >> 20) & 3u) != 0u ? 1u : 0u) | (clamp == 0u ? 2u : 0u) | (clamp == 1u ? 4u : 0u);\n"
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
