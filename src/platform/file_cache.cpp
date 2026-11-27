#include "platform/file_cache.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <cstdio>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace xbem::platform {
namespace {

constexpr uint64_t kBlock = 256ull << 10;

struct OpenFile {
  int fd = -1;
  uint32_t id = 0;
};

struct Block {
  std::vector<uint8_t> data;  // shorter than kBlock at the end of the file
};

struct Cache {
  std::mutex mutex;
  std::unordered_map<std::string, OpenFile> files;
  uint32_t next_id = 1;
  // LRU: most recent first.
  std::list<std::pair<uint64_t, std::shared_ptr<Block>>> lru;
  std::unordered_map<uint64_t, decltype(lru)::iterator> index;
  uint64_t bytes = 0, budget = 0;
  bool configured = false;
  // Statistics (logged at powers of two of disk reads).
  uint64_t requests = 0, requested = 0, disk_reads = 0, disk_bytes = 0, disk_ns = 0;

  void configure() {
    if (configured) return;
    configured = true;
    const char* v = std::getenv("XBEM_READ_CACHE_MB");
    budget = (v ? std::strtoull(v, nullptr, 10) : 128ull) << 20;
  }
};

Cache g_cache;

#if defined(_WIN32)
int64_t raw_read(int, uint64_t, void*, size_t) { return -1; }
int open_file(const std::string&) { return -1; }
#else
int64_t raw_read_uncounted(int fd, uint64_t offset, void* out, size_t n);
int64_t raw_read(int fd, uint64_t offset, void* out, size_t n) {
  const auto t0 = std::chrono::steady_clock::now();
  const int64_t r = raw_read_uncounted(fd, offset, out, n);
  const uint64_t ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
  uint64_t reads;
  {
    std::lock_guard lock(g_cache.mutex);
    reads = ++g_cache.disk_reads;
    g_cache.disk_bytes += r > 0 ? static_cast<uint64_t>(r) : 0;
    g_cache.disk_ns += ns;
  }
  if (reads >= 1024 && (reads & (reads - 1)) == 0)
    std::fprintf(stderr, "[info ][files] %llu disk reads, %llu MiB, %.1f s reading; %llu requests for %llu MiB\n",
                 static_cast<unsigned long long>(reads), static_cast<unsigned long long>(g_cache.disk_bytes >> 20),
                 g_cache.disk_ns / 1e9, static_cast<unsigned long long>(g_cache.requests),
                 static_cast<unsigned long long>(g_cache.requested >> 20));
  return r;
}
int64_t raw_read_uncounted(int fd, uint64_t offset, void* out, size_t n) {
  size_t done = 0;
  while (done < n) {
    const ssize_t r = ::pread(fd, static_cast<uint8_t*>(out) + done, n - done, static_cast<off_t>(offset + done));
    if (r < 0 && errno == EINTR) continue;  // a signal (thread dumps, faults skipped) - not the end of the data
    if (r < 0) return done ? static_cast<int64_t>(done) : -1;
    if (r == 0) break;
    done += static_cast<size_t>(r);
  }
  return static_cast<int64_t>(done);
}
int open_file(const std::string& path) { return ::open(path.c_str(), O_RDONLY | O_CLOEXEC); }
#endif

}  // namespace

void file_read_totals(uint64_t& requests, uint64_t& bytes) {
  std::lock_guard lock(g_cache.mutex);
  requests = g_cache.requests;
  bytes = g_cache.requested;
}

int64_t cached_read(const std::string& host_path, uint64_t offset, void* out, size_t n) {
  Cache& c = g_cache;
  OpenFile f;
  {
    std::lock_guard lock(c.mutex);
    c.configure();
    auto it = c.files.find(host_path);
    if (it == c.files.end()) {
      OpenFile o;
      o.fd = open_file(host_path);
      if (o.fd < 0) return -1;
      o.id = c.next_id++;
      it = c.files.emplace(host_path, o).first;
    }
    f = it->second;
  }
  {
    std::lock_guard lock(c.mutex);
    ++c.requests;
    c.requested += n;
  }
  // Large reads and a disabled cache go straight to the file.
  if (!c.budget || n >= kBlock * 2) return raw_read(f.fd, offset, out, n);
  size_t done = 0;
  while (done < n) {
    const uint64_t at = offset + done, block = at / kBlock, key = (uint64_t{f.id} << 40) | block;
    std::shared_ptr<Block> b;
    {
      std::lock_guard lock(c.mutex);
      if (auto it = c.index.find(key); it != c.index.end()) {
        c.lru.splice(c.lru.begin(), c.lru, it->second);
        b = it->second->second;
      }
    }
    if (!b) {
      b = std::make_shared<Block>();
      b->data.resize(kBlock);
      const int64_t got = raw_read(f.fd, block * kBlock, b->data.data(), kBlock);
      if (got < 0) return done ? static_cast<int64_t>(done) : -1;
      b->data.resize(static_cast<size_t>(got));
      std::lock_guard lock(c.mutex);
      if (c.index.find(key) == c.index.end()) {
        c.lru.emplace_front(key, b);
        c.index[key] = c.lru.begin();
        c.bytes += b->data.size();
        while (c.bytes > c.budget && c.lru.size() > 1) {
          c.bytes -= c.lru.back().second->data.size();
          c.index.erase(c.lru.back().first);
          c.lru.pop_back();
        }
      }
    }
    const uint64_t in_block = at - block * kBlock;
    if (in_block >= b->data.size()) break;  // end of file
    const size_t take = static_cast<size_t>(std::min<uint64_t>(n - done, b->data.size() - in_block));
    std::memcpy(static_cast<uint8_t*>(out) + done, b->data.data() + in_block, take);
    done += take;
    if (b->data.size() < kBlock) break;
  }
  return static_cast<int64_t>(done);
}

}  // namespace xbem::platform
