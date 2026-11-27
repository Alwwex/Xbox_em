#include <bit>
#include <cstdio>
#include "gpu/vk/guest_memory.h"

#include <algorithm>
#include <cstring>
#include <format>

#include "common/log.h"
#include "gpu/guest_memory_layout.h"
#include "platform/page_tracker.h"

namespace xbem::gpu::vk {

namespace pg = platform::pages;
using namespace gpu::mem;

namespace {
constexpr uint64_t kPage = pg::kPageSize;
constexpr VkBufferUsageFlags kUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT;
}  // namespace

bool GuestMemory::init(Core& core, uint64_t budget_bytes, std::string& error) {
  core_ = &core;
  table_ = core.create_buffer(uint64_t{kTableWords} * 4, kUsage, true);
  if (!table_.mapped) {
    error = "cannot allocate the guest memory table";
    return false;
  }
  std::memset(table_.mapped, 0, uint64_t{kTableWords} * 4);
  budget_pages_ = static_cast<uint32_t>(
      std::clamp<uint64_t>(budget_bytes / kPage, 2 * kChunkPages, uint64_t{kMaxChunks} * kChunkPages));
  pages_.emplace_back();  // the scratch page
  if (!allocate_chunk_for(0)) {
    error = "cannot allocate guest memory pool";
    table_ = {};
    return false;
  }
  scratch_.resize(kPage);
  return true;
}

bool GuestMemory::allocate_chunk_for(uint32_t idx) {
  const uint32_t c = idx / kChunkPages;
  while (chunks_.size() <= c) {
    Buffer b = core_->create_buffer(uint64_t{kChunkPages} * kPage, kUsage, true);
    if (!b.mapped) {
      XBEM_WARN("gpu", "cannot allocate guest memory chunk {} ({} MiB)", chunks_.size(), kChunkPages * kPage >> 20);
      return false;
    }
    const uint32_t k = static_cast<uint32_t>(chunks_.size());
    words()[k * 2] = static_cast<uint32_t>(b.address);
    words()[k * 2 + 1] = static_cast<uint32_t>(b.address >> 32);
    chunks_.push_back(b);
  }
  return true;
}

uint8_t* GuestMemory::page_data(uint32_t idx) const {
  return static_cast<uint8_t*>(chunks_[idx / kChunkPages].mapped) + uint64_t{idx % kChunkPages} * kPage;
}

uint32_t* GuestMemory::entry(uint64_t va, bool create) {
  if (va >= (1ull << 40)) return nullptr;
  uint32_t& dir = words()[kDirWord + (va >> 26)];
  if (!dir) {
    if (!create) return nullptr;
    if (leaves_ >= kMaxLeaves) {
      if (!leaves_full_logged_) XBEM_WARN("gpu", "guest memory table full ({} leaves)", kMaxLeaves);
      leaves_full_logged_ = true;
      return nullptr;
    }
    dir = leaves_++;
  }
  return &words()[kLeafWord + dir * kLeafEntries + ((va >> 16) & (kLeafEntries - 1))];
}

uint32_t GuestMemory::allocate() {
  if (!free_.empty()) {
    const uint32_t idx = free_.back();
    free_.pop_back();
    return idx;
  }
  if (pages_.size() < budget_pages_) {
    const auto idx = static_cast<uint32_t>(pages_.size());
    if (allocate_chunk_for(idx)) {
      pages_.emplace_back();
      return idx;
    }
    budget_pages_ = static_cast<uint32_t>(pages_.size());  // the host is out of memory: work with what there is
  }
  // Full: drop the page unused for longest (never one this submission uses).
  uint32_t best = 0;
  for (uint32_t i = 1; i < pages_.size(); ++i)
    if (pages_[i].id && pages_[i].last_use < serial_ && (!best || pages_[i].last_use < pages_[best].last_use)) best = i;
  if (!best) {
    if (!budget_logged_)
      XBEM_WARN("gpu", "guest memory budget ({} MiB) used up by one submission - more is read as zeros",
                uint64_t{budget_pages_} * kPage >> 20);
    budget_logged_ = true;
    return 0;
  }
  release(best);
  ++stats_.evicted;
  free_.pop_back();  // release() put it there
  return best;
}

void GuestMemory::release(uint32_t idx) {
  Page& p = pages_[idx];
  for (uint64_t va : p.vas)
    if (uint32_t* e = entry(va, false); e && *e == idx) *e = 0;
  if (p.id) {
    pg::unwatch(p.id);
    by_id_.erase(p.id);
  }
  words()[kWrittenWord + idx / 2] &= idx & 1 ? 0x0000ffffu : 0xffff0000u;
  p = Page{};
  free_.push_back(idx);
  --stats_.resident;
}

bool GuestMemory::require(uint64_t va, uint64_t size) {
  if (!ready() || !size) return false;
  bool any = false;
  for (uint64_t p = va & ~(kPage - 1); p < va + size; p += kPage) {
    uint32_t* e = entry(p, true);
    if (!e) return any;
    if (*e) {
      pages_[*e].last_use = serial_;
      any = true;
      continue;
    }
    const uint64_t id = pg::page_id(p);
    if (!id) continue;  // nothing mapped: the scratch page
    uint32_t idx;
    if (auto it = by_id_.find(id); it != by_id_.end()) {
      idx = it->second;
    } else {
      idx = allocate();
      if (!idx) continue;
      pages_[idx].id = id;
      by_id_[id] = idx;
      ++stats_.resident;
      pg::watch(id);  // before the copy: a write during it is seen at the next submission
      pg::read_page(id, page_data(idx));
      ++stats_.uploaded;
    }
    *e = idx;
    pages_[idx].vas.push_back(p);
    pages_[idx].last_use = serial_;
    any = true;
  }
  return any;
}

void GuestMemory::forget_range(uint64_t va, uint64_t size) {
  const uint64_t end = std::min<uint64_t>(va + size, 1ull << 40);
  for (uint64_t p = va & ~(kPage - 1); p < end;) {
    if (!words()[kDirWord + (p >> 26)]) {  // no leaf: skip to the next one
      p = (p | ((1ull << 26) - 1)) + 1;
      continue;
    }
    if (uint32_t* e = entry(p, false); e && *e) {
      const uint32_t idx = *e;
      *e = 0;
      auto& vas = pages_[idx].vas;
      vas.erase(std::remove(vas.begin(), vas.end(), p), vas.end());
      // An anonymous page's contents went with its address; a physical page
      // stays while other addresses show it.
      if (vas.empty() || !(pages_[idx].id & pg::kSharedId)) release(idx);
      ++stats_.unmapped;
    }
    p += kPage;
  }
}

void GuestMemory::alias(uint64_t window, uint64_t real, uint64_t size) {
  if (!ready() || ((window ^ real) & (kPage - 1))) return;
  aliases_.push_back({window, real, size});
  map_alias(aliases_.back());
}

void GuestMemory::map_alias(const Alias& a) {
  for (uint64_t w = a.window & ~(kPage - 1); w < a.window + a.size; w += kPage) {
    const uint64_t r = a.real - (a.window - w);  // the real page's start
    require(std::max(r, a.real), 1);
    const uint32_t* re = entry(r, false);
    if (!re || !*re) continue;
    uint32_t* we = entry(w, true);
    if (!we || *we == *re) continue;
    if (*we) {
      auto& vas = pages_[*we].vas;
      vas.erase(std::remove(vas.begin(), vas.end(), w), vas.end());
    }
    *we = *re;
    pages_[*re].vas.push_back(w);
  }
}

// Pages of [va, va + size) may be reported as misses again.
void GuestMemory::clear_missed(uint64_t va, uint64_t size) {
  const uint64_t end = std::min<uint64_t>(va + size, 1ull << 40);
  for (uint64_t page = va >> 16; page < (end + kPage - 1) >> 16;) {
    uint32_t& w = words()[kMissedWord + page / 32];
    if (page % 32 == 0 && (page + 32) << 16 <= end) {
      w = 0;
      page += 32;
    } else {
      w &= ~(1u << (page % 32));
      ++page;
    }
  }
}

void GuestMemory::begin_submission() {
  if (!ready()) return;
  ++serial_;
  pg::Changes changes;
  pg::collect(changes);
  changes.remapped.insert(changes.remapped.begin(), pending_.remapped.begin(), pending_.remapped.end());
  changes.written.insert(changes.written.begin(), pending_.written.begin(), pending_.written.end());
  pending_.remapped.clear();
  pending_.written.clear();
  for (const auto& [va, size] : changes.remapped) {
    forget_range(va, size);
    clear_missed(va, size);
  }
  for (const auto& [id, mask] : changes.written) {
    auto it = by_id_.find(id);
    if (it == by_id_.end()) continue;
    uint8_t* dst = page_data(it->second);
    if (mask == 0xffff) {
      pg::read_page(id, dst);
    } else {
      pg::read_page(id, scratch_.data());
      for (uint32_t part = 0; part < 16; ++part)
        if (mask & (1u << part))
          std::memcpy(dst + part * pg::kPartSize, scratch_.data() + part * pg::kPartSize, pg::kPartSize);
    }
    ++stats_.refreshed;
  }
  for (const Alias& a : aliases_) map_alias(a);
}

void GuestMemory::end_submission() {
  if (!ready()) return;
  // Pages shaders reached without a copy: copied in for the next submission.
  // A page that is not guest memory keeps its bit (it is not reported again
  // until a mapping changes there).
  {
    uint32_t* w = words();
    const uint32_t n = std::min(w[kMissWord], kMaxMisses);
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t page = w[kMissWord + 1 + i];
      if (require(uint64_t{page} << 16, kPage)) w[kMissedWord + page / 32] &= ~(1u << (page % 32));
    }
    stats_.misses += n;
    if (w[kMissWord] > kMaxMisses) stats_.misses_dropped += w[kMissWord] - kMaxMisses;
    w[kMissWord] = 0;
  }
  // Parts the guest's CPU wrote while the GPU ran: a page copied back whole
  // would undo those writes (GTA VI keeps CPU objects in pages it also maps
  // for the GPU; restored stale bytes crashed its worker threads). Without a
  // snapshot to merge with, the CPU's bytes win there.
  pg::Changes during;
  pg::collect(during);
  std::unordered_map<uint64_t, uint16_t> cpu_wrote;
  for (const auto& [id, mask] : during.written) cpu_wrote[id] |= mask;
  pending_.remapped.insert(pending_.remapped.end(), during.remapped.begin(), during.remapped.end());
  pending_.written.insert(pending_.written.end(), during.written.begin(), during.written.end());
  uint32_t* w = words() + kWrittenWord;
  const size_t n = (pages_.size() + 1) / 2;
  for (size_t i = 0; i < n; ++i) {
    if (!w[i]) continue;
    for (uint32_t h = 0; h < 2; ++h) {
      const auto idx = static_cast<uint32_t>(i * 2 + h);
      const auto mask = static_cast<uint16_t>(w[i] >> (h * 16));
      if (!mask || !idx || idx >= pages_.size() || !pages_[idx].id) continue;
      if (on_write_back && !pages_[idx].vas.empty()) on_write_back(pages_[idx].vas.front(), mask);
      if (auto it = snapshots_.find(idx); it != snapshots_.end()) {
        // Three-way: the guest's bytes now, with the words the GPU changed.
        const uint8_t* gpu = page_data(idx);
        const uint8_t* before = it->second.data();
        pg::read_page(pages_[idx].id, scratch_.data());
        uint16_t changed = 0;
        for (uint32_t part = 0; part < 16; ++part) {
          if (!(mask & (1u << part))) continue;
          const size_t at = size_t{part} * pg::kPartSize;
          for (size_t k = at; k < at + pg::kPartSize; k += 4) {
            if (std::memcmp(gpu + k, before + k, 4) == 0) continue;
            std::memcpy(scratch_.data() + k, gpu + k, 4);
            changed |= static_cast<uint16_t>(1u << part);
          }
        }
        if (changed) pg::write_page(pages_[idx].id, scratch_.data(), changed);
        ++stats_.merged;
      } else {
        uint16_t keep = mask;
        if (auto c = cpu_wrote.find(pages_[idx].id); c != cpu_wrote.end() && (c->second & mask)) {
          keep = static_cast<uint16_t>(mask & ~c->second);
          if (++stats_.conflicts <= 16 || std::has_single_bit(stats_.conflicts))
            std::fprintf(stderr, "[warn ][gpu] page %#llx: parts %#x written by both the GPU and the CPU during a "
                         "submission - the CPU's bytes kept (#%llu)\n",
                         static_cast<unsigned long long>(pages_[idx].id), c->second & mask,
                         static_cast<unsigned long long>(stats_.conflicts));
        }
        if (keep) pg::write_page(pages_[idx].id, page_data(idx), keep);
      }
      ++stats_.written_back;
    }
    w[i] = 0;
  }
  snapshots_.clear();
  snapshot_bytes_ = 0;
}

void GuestMemory::snapshot(uint64_t va, uint64_t size) {
  if (!ready() || !size) return;
  constexpr uint64_t kMaxSnapshotBytes = 256ull << 20;
  const uint64_t end = std::min<uint64_t>(va + size, 1ull << 47);
  for (uint64_t p = va & ~(kPage - 1); p < end; p += kPage) {
    const uint32_t* e = entry(p, false);
    if (!e || !*e || snapshots_.count(*e)) continue;
    if (snapshot_bytes_ + kPage > kMaxSnapshotBytes) return;  // the rest is written back by parts
    const uint8_t* d = page_data(*e);
    snapshots_.emplace(*e, std::vector<uint8_t>(d, d + kPage));
    snapshot_bytes_ += kPage;
  }
}

}  // namespace xbem::gpu::vk
