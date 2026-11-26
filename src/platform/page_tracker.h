#pragma once

// Which memory backs each guest page, and which of the pages a consumer (the
// GPU memory mirror) holds a copy of have been written since.
//
// Guest memory below 2^40 (the addresses a console GPU sees) is either
// anonymous (VirtualAlloc & co.: its contents belong to the virtual address)
// or a view of the physical memory object (XMemMapPhysicalPages: the same
// 64 KiB page can be mapped at several addresses). A page id names the
// contents: an anonymous page's id is its 64 KiB aligned address, a physical
// page's id is kSharedId | its byte offset in the physical memory object.
// platform/memory.cpp reports every mapping change here.
//
// Writes are detected with userfaultfd write protection in asynchronous mode
// and PAGEMAP_SCAN (Linux 6.7+, no privileges needed): the kernel marks a
// page written on the first write after it was protected - writes by system
// calls (file reads into guest buffers) included - and a scan returns and
// re-protects the written pages atomically. Only the addresses where watched
// pages are mapped are protected and scanned, so the rest of guest memory
// never takes a write fault. Without userfaultfd (other hosts, or
// XBEM_NO_WRITE_TRACKING=1) every watched page counts as written at every
// collect.

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace xbem::platform::pages {

constexpr uint64_t kPageSize = 0x10000;   // the unit of ids (64 KiB, the console's page)
constexpr uint64_t kPartSize = 0x1000;    // written state is kept per 4 KiB part
constexpr uint64_t kLimit = 1ull << 40;   // tracked guest addresses: [0, kLimit)
constexpr uint64_t kSharedId = 1ull << 63;

// --- reported by platform/memory.cpp (ranges are 4 KiB aligned) ---
void mapped_anonymous(uint64_t va, uint64_t size);  // fresh (zero) or reset contents
void mapped_shared(uint64_t va, uint64_t size, intptr_t memory, uint64_t offset);
void unmapped(uint64_t va, uint64_t size);
void shared_written(uint64_t offset, uint64_t size);  // written in the object itself (not through a mapping)

// --- consumer side ---
// False when writes cannot be detected (every watched page is then reported
// written at every collect).
bool write_tracking();
// The id of the page at `va`; 0 when nothing is mapped there.
uint64_t page_id(uint64_t va);
// Starts reporting writes to page `id` (protects the addresses it is mapped
// at). Call before copying its contents, so no write is missed.
void watch(uint64_t id);
void unwatch(uint64_t id);

struct Changes {
  // Address ranges whose backing changed since the last collect (anonymous
  // pages there have new contents; physical views may name another page).
  std::vector<std::pair<uint64_t, uint64_t>> remapped;
  // Watched pages written since the last collect: id, 4 KiB parts (bit n).
  std::vector<std::pair<uint64_t, uint16_t>> written;
};
// Moves what changed since the last call into `out` (cleared first).
void collect(Changes& out);

// Contents of page `id` (64 KiB; unmapped parts read as zero).
bool read_page(uint64_t id, void* out);
// Writes the parts in `mask` of page `id` (the GPU wrote them). Physical
// pages are written in the object (not reported); anonymous pages through
// their address, so they are reported written at the next collect - the
// consumer then re-reads the whole page, with any guest writes made while
// the GPU ran.
bool write_page(uint64_t id, const void* data, uint16_t mask);

// For tests: forget all state (mappings stay as they are).
void reset_for_tests();

}  // namespace xbem::platform::pages
