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

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "gpu/vk/vk_core.h"

namespace xbem::gpu::vk {

class GuestMemory {
 public:
  bool init(Core& core, uint64_t budget_bytes, std::string& error);
  bool ready() const { return table_.mapped != nullptr; }
  // The table's device address (the shaders' pc.mem).
  VkDeviceAddress table_address() const { return table_.address; }

  // Start of a submission: guest writes and remappings since the last one.
  void begin_submission();
  // Makes [va, va + size) readable on the GPU. False when (part of) it is
  // not tracked guest memory (the caller then copies it some other way).
  bool require(uint64_t va, uint64_t size);
  // The submission has completed: GPU writes go back to guest memory.
  void end_submission();

  struct Stats {
    uint64_t resident = 0, uploaded = 0, refreshed = 0, written_back = 0, evicted = 0, unmapped = 0;
  };
  const Stats& stats() const { return stats_; }

 private:
  struct Page {
    uint64_t id = 0;  // platform::pages id; 0 = free
    uint64_t last_use = 0;
    std::vector<uint64_t> vas;  // 64 KiB guest pages whose table entry names this page
  };

  uint32_t* words() const { return static_cast<uint32_t*>(table_.mapped); }
  uint8_t* page_data(uint32_t idx) const;
  uint32_t* entry(uint64_t va, bool create);
  bool allocate_chunk_for(uint32_t idx);
  uint32_t allocate();
  void release(uint32_t idx);
  void forget_range(uint64_t va, uint64_t size);

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
