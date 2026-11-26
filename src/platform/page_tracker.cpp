#include "platform/page_tracker.h"

#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "common/log.h"
#include "platform/memory.h"

#if defined(__linux__)
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace xbem::platform::pages {
namespace {

#if defined(__linux__) && defined(PAGEMAP_SCAN) && defined(UFFD_FEATURE_WP_ASYNC)
#define XBEM_PAGE_TRACKING 1
#endif

struct Segment {
  uint64_t end = 0;
  bool shared = false;
  uint64_t offset = 0;  // shared: offset of the segment's start in the object
};

struct State {
  std::mutex mutex;
  std::map<uint64_t, Segment> segments;  // start -> segment, non-overlapping
  intptr_t shared_memory = -1;
  std::unordered_map<uint64_t, std::vector<uint64_t>> views;  // shared id -> 64 KiB pages mapping it
  std::unordered_set<uint64_t> watched;
  std::set<uint64_t> watched_vas;  // 64 KiB pages where watched ids are mapped
  bool consumer = false;           // watch() was called: record changes
  std::vector<std::pair<uint64_t, uint64_t>> remapped;
  std::unordered_map<uint64_t, uint16_t> written;
  bool tried = false, tracking = false;
  int uffd = -1, pagemap = -1;
};

State& state() {
  static State s;
  return s;
}

void init_tracking(State& s) {
  if (s.tried) return;
  s.tried = true;
#if defined(XBEM_PAGE_TRACKING)
  if (const char* v = std::getenv("XBEM_NO_WRITE_TRACKING"); v && *v && *v != '0') return;
  const int fd = static_cast<int>(syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY));
  if (fd < 0) {
    XBEM_WARN("pages", "userfaultfd unavailable ({}) - GPU copies of guest memory are refreshed every time",
              std::strerror(errno));
    return;
  }
  uffdio_api api{};
  api.api = UFFD_API;
  api.features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_HUGETLBFS_SHMEM | UFFD_FEATURE_WP_UNPOPULATED;
  const int pm = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
  if (ioctl(fd, UFFDIO_API, &api) != 0 || pm < 0) {
    XBEM_WARN("pages", "asynchronous userfaultfd write protection unavailable - GPU copies are refreshed every time");
    close(fd);
    if (pm >= 0) close(pm);
    return;
  }
  s.uffd = fd;
  s.pagemap = pm;
  s.tracking = true;
#endif
}

void register_range(State& s, uint64_t va, uint64_t size) {
  init_tracking(s);
#if defined(XBEM_PAGE_TRACKING)
  if (!s.tracking) return;
  uffdio_register reg{};
  reg.range.start = va;
  reg.range.len = size;
  reg.mode = UFFDIO_REGISTER_MODE_WP;
  if (ioctl(s.uffd, UFFDIO_REGISTER, &reg) != 0) {
    // An unregistered range would hide writes: stop trusting the tracking.
    XBEM_WARN("pages", "cannot watch writes to {:#x}+{:#x} ({}) - write tracking off", va, size, std::strerror(errno));
    s.tracking = false;
  }
#else
  (void)va, (void)size;
#endif
}

// Scans [va, va+size) for pages written since they were protected, protects
// them again, and calls `fn(address, size)` for each written run.
template <typename Fn>
void scan_written(State& s, uint64_t va, uint64_t size, Fn&& fn) {
#if defined(XBEM_PAGE_TRACKING)
  page_region regions[256];
  pm_scan_arg arg{};
  arg.size = sizeof arg;
  arg.flags = PM_SCAN_WP_MATCHING;
  arg.start = va;
  arg.end = va + size;
  arg.vec = reinterpret_cast<uint64_t>(regions);
  arg.vec_len = 256;
  arg.category_mask = PAGE_IS_WRITTEN;
  arg.return_mask = PAGE_IS_WRITTEN;
  while (arg.start < arg.end) {
    const long n = ioctl(s.pagemap, PAGEMAP_SCAN, &arg);
    if (n < 0) {
      XBEM_WARN("pages", "PAGEMAP_SCAN {:#x}+{:#x}: {} - write tracking off", va, size, std::strerror(errno));
      s.tracking = false;
      return;
    }
    for (long i = 0; i < n; ++i) fn(regions[i].start, regions[i].end - regions[i].start);
    if (arg.walk_end <= arg.start || arg.walk_end >= arg.end) break;
    arg.start = arg.walk_end;
  }
#else
  (void)s, (void)va, (void)size, (void)fn;
#endif
}

// The segment holding `va`, or segments.end().
std::map<uint64_t, Segment>::iterator find(State& s, uint64_t va) {
  auto it = s.segments.upper_bound(va);
  if (it == s.segments.begin()) return s.segments.end();
  --it;
  return va < it->second.end ? it : s.segments.end();
}

uint64_t id_at(State& s, uint64_t va) {
  auto it = find(s, va);
  if (it == s.segments.end()) return 0;
  if (!it->second.shared) return va & ~(kPageSize - 1);
  return kSharedId | ((it->second.offset + (va - it->first)) & ~(kPageSize - 1));
}

void drop_view(State& s, uint64_t id, uint64_t page) {
  auto v = s.views.find(id);
  if (v == s.views.end()) return;
  auto& list = v->second;
  for (size_t i = 0; i < list.size(); ++i)
    if (list[i] == page) {
      list[i] = list.back();
      list.pop_back();
      break;
    }
  if (list.empty()) s.views.erase(v);
}

// Removes [start, end) from the segment map (splitting segments at the edges)
// and forgets the physical views there.
void cut(State& s, uint64_t start, uint64_t end) {
  auto it = s.segments.upper_bound(start);
  if (it != s.segments.begin() && std::prev(it)->second.end > start) --it;
  while (it != s.segments.end() && it->first < end) {
    const uint64_t a = it->first;
    Segment seg = it->second;
    it = s.segments.erase(it);
    if (a < start) {
      Segment head = seg;
      head.end = start;
      s.segments[a] = head;
    }
    if (seg.end > end) {
      Segment tail = seg;
      if (tail.shared) tail.offset += end - a;
      s.segments[end] = tail;
    }
    if (seg.shared) {
      const uint64_t from = std::max(a, start), to = std::min(seg.end, end);
      for (uint64_t p = from & ~(kPageSize - 1); p < to; p += kPageSize) {
        const uint64_t id = kSharedId | ((seg.offset + (std::max(p, a) - a)) & ~(kPageSize - 1));
        drop_view(s, id, p);
      }
    }
    it = s.segments.upper_bound(a);
  }
}

// Recomputes which pages of [start, end) are scanned for writes.
void refresh_watched(State& s, uint64_t start, uint64_t end) {
  if (s.watched.empty()) return;
  for (uint64_t p = start & ~(kPageSize - 1); p < end; p += kPageSize) {
    const uint64_t id = id_at(s, p);
    if (id && s.watched.count(id)) s.watched_vas.insert(p);
    else s.watched_vas.erase(p);
  }
}

void note_remap(State& s, uint64_t va, uint64_t size) {
  if (s.consumer) s.remapped.emplace_back(va, size);
}

uint16_t parts_mask(uint64_t from, uint64_t to) {  // parts of one page touched by [from, to) (offsets in the page)
  uint16_t m = 0;
  for (uint64_t p = from / kPartSize; p * kPartSize < to && p < 16; ++p) m |= static_cast<uint16_t>(1u << p);
  return m;
}

bool tracked(uint64_t va, uint64_t size) { return va < kLimit && size && va + size <= kLimit; }

}  // namespace

void mapped_anonymous(uint64_t va, uint64_t size) {
  if (!tracked(va, size)) return;
  State& s = state();
  std::lock_guard lock(s.mutex);
  cut(s, va, va + size);
  s.segments[va] = Segment{va + size, false, 0};
  register_range(s, va, size);
  refresh_watched(s, va, va + size);
  note_remap(s, va, size);
}

void mapped_shared(uint64_t va, uint64_t size, intptr_t memory, uint64_t offset) {
  if (!tracked(va, size)) return;
  State& s = state();
  std::lock_guard lock(s.mutex);
  s.shared_memory = memory;
  cut(s, va, va + size);
  s.segments[va] = Segment{va + size, true, offset};
  for (uint64_t p = va & ~(kPageSize - 1); p < va + size; p += kPageSize)
    s.views[kSharedId | ((offset + (std::max(p, va) - va)) & ~(kPageSize - 1))].push_back(p);
  register_range(s, va, size);
  refresh_watched(s, va, va + size);
  note_remap(s, va, size);
}

void unmapped(uint64_t va, uint64_t size) {
  if (!tracked(va, size)) return;
  State& s = state();
  std::lock_guard lock(s.mutex);
  cut(s, va, va + size);
  refresh_watched(s, va, va + size);
  note_remap(s, va, size);
}

void shared_written(uint64_t offset, uint64_t size) {
  State& s = state();
  std::lock_guard lock(s.mutex);
  if (s.watched.empty()) return;
  for (uint64_t p = offset & ~(kPageSize - 1); p < offset + size; p += kPageSize) {
    const uint64_t id = kSharedId | p;
    if (!s.watched.count(id)) continue;
    const uint64_t from = std::max(p, offset) - p, to = std::min(p + kPageSize, offset + size) - p;
    s.written[id] |= parts_mask(from, to);
  }
}

bool write_tracking() {
  State& s = state();
  std::lock_guard lock(s.mutex);
  init_tracking(s);
  return s.tracking;
}

uint64_t page_id(uint64_t va) {
  State& s = state();
  std::lock_guard lock(s.mutex);
  return va < kLimit ? id_at(s, va) : 0;
}

void watch(uint64_t id) {
  State& s = state();
  std::lock_guard lock(s.mutex);
  s.consumer = true;
  if (!id || !s.watched.insert(id).second) return;
  auto arm = [&](uint64_t page) {
    s.watched_vas.insert(page);
    if (s.tracking) scan_written(s, page, kPageSize, [](uint64_t, uint64_t) {});
  };
  if (id & kSharedId) {
    if (auto v = s.views.find(id); v != s.views.end())
      for (uint64_t page : v->second) arm(page);
  } else if (id_at(s, id) == id) {
    arm(id);
  }
}

void unwatch(uint64_t id) {
  State& s = state();
  std::lock_guard lock(s.mutex);
  if (!s.watched.erase(id)) return;
  s.written.erase(id);
  if (id & kSharedId) {
    if (auto v = s.views.find(id); v != s.views.end())
      for (uint64_t page : v->second) s.watched_vas.erase(page);
  } else {
    s.watched_vas.erase(id);
  }
}

void collect(Changes& out) {
  out.remapped.clear();
  out.written.clear();
  State& s = state();
  std::lock_guard lock(s.mutex);
  init_tracking(s);
  if (s.tracking) {
    // Contiguous runs of watched pages, one scan each.
    for (auto it = s.watched_vas.begin(); it != s.watched_vas.end();) {
      const uint64_t start = *it;
      uint64_t end = start + kPageSize;
      for (++it; it != s.watched_vas.end() && *it == end; ++it) end += kPageSize;
      scan_written(s, start, end - start, [&](uint64_t a, uint64_t n) {
        for (uint64_t p = a & ~(kPageSize - 1); p < a + n; p += kPageSize) {
          const uint64_t id = id_at(s, p);
          if (!id || !s.watched.count(id)) continue;
          s.written[id] |= parts_mask(std::max(p, a) - p, std::min(p + kPageSize, a + n) - p);
        }
      });
    }
  }
  if (!s.tracking)
    for (uint64_t id : s.watched) s.written[id] = 0xffff;
  out.remapped.swap(s.remapped);
  out.written.assign(s.written.begin(), s.written.end());
  s.written.clear();
}

bool read_page(uint64_t id, void* out) {
  auto* dst = static_cast<uint8_t*>(out);
  intptr_t memory;
  {
    State& s = state();
    std::lock_guard lock(s.mutex);
    memory = s.shared_memory;
  }
  if (id & kSharedId) {
#if defined(__linux__)
    uint64_t done = 0;
    while (done < kPageSize) {
      const ssize_t n = pread(static_cast<int>(memory), dst + done, kPageSize - done,
                              static_cast<off_t>((id & ~kSharedId) + done));
      if (n <= 0) return false;
      done += static_cast<uint64_t>(n);
    }
    return true;
#else
    return false;
#endif
  }
  if (!safe_read(id, dst, kPageSize))
    for (uint64_t p = 0; p < kPageSize; p += kPartSize)
      if (!safe_read(id + p, dst + p, kPartSize)) std::memset(dst + p, 0, kPartSize);
  return true;
}

bool write_page(uint64_t id, const void* data, uint16_t mask) {
  const auto* src = static_cast<const uint8_t*>(data);
  State& s = state();
  bool ok = true;
  for (uint32_t part = 0; part < 16; ++part) {
    if (!(mask & (1u << part))) continue;
    const uint64_t at = part * kPartSize;
    if (id & kSharedId) {
      intptr_t memory;
      {
        std::lock_guard lock(s.mutex);
        memory = s.shared_memory;
      }
      ok &= write_shared(memory, (id & ~kSharedId) + at, src + at, kPartSize, /*report=*/false);
    } else {
      ok &= safe_write(id + at, src + at, kPartSize);
    }
  }
  return ok;
}

void reset_for_tests() {
  State& s = state();
  std::lock_guard lock(s.mutex);
  s.segments.clear();
  s.views.clear();
  s.watched.clear();
  s.watched_vas.clear();
  s.consumer = false;
  s.remapped.clear();
  s.written.clear();
}

}  // namespace xbem::platform::pages
