#pragma once

// The GPU's copy of guest memory: 64 KiB pages in a pool of host-visible
// Vulkan memory and the table translated shaders look addresses up in
// (layout in gpu/guest_memory_layout.h). Guest pages are copied in when a
// draw or dispatch needs them (require), refreshed at the start of every
// submission when the guest wrote them (platform/page_tracker.h detects the
// writes), and pages the GPU wrote are copied back to guest memory when the
// submission has finished. Physical pages mapped at several addresses have
// one copy, as on the console.
//
// The pool is host-visible (system memory the GPU reads over the bus): the
// copies are plain memcpy, and GPU writes are visible to the CPU at once.
// When the budget is used up, the pages unused for longest are dropped.
// Pages shaders reach without a copy (descriptors read on the GPU name
// buffers the CPU never saw) are reported by the shaders and copied in after
// the submission, so from the next one on they read right.

#include "platform/page_tracker.h"
#include <functional>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "gpu/guest_memory_layout.h"
#include "gpu/vk/vk_core.h"

namespace xbem::gpu::vk {

class GuestMemory {
 public:
  bool init(Core& core, uint64_t budget_bytes, std::string& error);
  bool ready() const { return table_.mapped != nullptr; }
  // The table's device address (the shaders' pc.mem).
  VkDeviceAddress table_address() const { return table_.address; }
  // The table itself (layout in gpu/guest_memory_layout.h), for the parts the
  // renderer keeps (the descriptor table).
  uint32_t* table_words() const { return words(); }

  // Start of a submission: guest writes and remappings since the last one.
  void begin_submission();
  // Makes [va, va + size) readable on the GPU. False when (part of) it is
  // not tracked guest memory (the caller then copies it some other way).
  bool require(uint64_t va, uint64_t size);
  // Before the submission runs: the GPU copies of [va, va + size) as they
  // are now (the ranges its dispatches write). Written-back parts of these
  // pages take only the 32-bit words the GPU changed, so guest writes made
  // meanwhile to the rest of a 4 KiB part (other threads run while the GPU
  // does) are not reverted.
  void snapshot(uint64_t va, uint64_t size);
  // The submission has completed: GPU writes go back to guest memory.
  void end_submission();
  // Makes [window, window + size) show guest memory [real, real + size) on
  // the GPU (window and real must agree modulo 64 KiB), kept up to date with
  // remappings of `real`. Descriptor heaps get windows below 4 GiB: the
  // console's shaders build table addresses from 32 bits.
  void alias(uint64_t window, uint64_t real, uint64_t size);

  struct Stats {
    uint64_t resident = 0, uploaded = 0, refreshed = 0, written_back = 0, merged = 0, evicted = 0, unmapped = 0,
             conflicts = 0;
    uint64_t misses = 0, misses_dropped = 0;  // pages shaders reached without a copy
  };
  const Stats& stats() const { return stats_; }
  // Called for each page the GPU wrote before it is copied back (a guest
  // address it is mapped at, the 4 KiB parts) - for diagnostics.
  std::function<void(uint64_t va, uint16_t mask)> on_write_back;
  // The tag a compute shader left when its loop guard ended it (0: none);
  // cleared. Call after a submission finished.
  uint32_t take_guard_tag(uint32_t* block = nullptr) {
    if (!table_.mapped) return 0;
    uint32_t& w = words()[gpu::mem::kGuardWord];
    const uint32_t tag = w;
    w = 0;
    if (block) *block = words()[gpu::mem::kGuardBlockWord];
    return tag;
  }

  // XBEM_PS_STATS counters (max, sum, count of blocks, tag of the max); cleared.
  void take_block_stats(uint32_t out[4]) {
    for (int i = 0; i < 4; ++i) out[i] = 0;
    if (!table_.mapped) return;
    for (int i = 0; i < 4; ++i) out[i] = words()[gpu::mem::kStatWord + i], words()[gpu::mem::kStatWord + i] = 0;
  }

 private:
  struct Page {
    uint64_t id = 0;  // platform::pages id; 0 = free
    uint64_t last_use = 0;
    std::vector<uint64_t> vas;  // 64 KiB guest pages whose table entry names this page
  };

  std::unordered_map<uint32_t, std::vector<uint8_t>> snapshots_;  // page index -> its copy before the GPU ran
  // What changed while the GPU ran (collected at the end of a submission to
  // find parts both sides wrote; handled at the next begin_submission).
  xbem::platform::pages::Changes pending_;
  uint64_t snapshot_bytes_ = 0;

  uint32_t* words() const { return static_cast<uint32_t*>(table_.mapped); }
  uint8_t* page_data(uint32_t idx) const;
  uint32_t* entry(uint64_t va, bool create);
  bool allocate_chunk_for(uint32_t idx);
  uint32_t allocate();
  void release(uint32_t idx);
  void forget_range(uint64_t va, uint64_t size);
  void clear_missed(uint64_t va, uint64_t size);
  struct Alias {
    uint64_t window, real, size;
  };
  void map_alias(const Alias& a);
  std::vector<Alias> aliases_;

  Core* core_ = nullptr;
  Buffer table_;
  std::vector<Buffer> chunks_;
  std::vector<Page> pages_;  // by pool index; [0] is the scratch page
  std::vector<uint32_t> free_;
  std::unordered_map<uint64_t, uint32_t> by_id_;
  uint32_t leaves_ = 1;  // leaf 0 is "none"
  uint32_t budget_pages_ = 0;
  uint64_t serial_ = 1;
  bool leaves_full_logged_ = false, budget_logged_ = false;
  Stats stats_;
  std::vector<uint8_t> scratch_;
};

}  // namespace xbem::gpu::vk
