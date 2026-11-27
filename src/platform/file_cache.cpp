#include "platform/file_cache.h"

#include <algorithm>
#include <atomic>
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
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace xbem::platform {
namespace {

// Block size: GTA VI's scene reads are mostly 16-512 KiB at random offsets;
// 256 KiB blocks read 4.2x the requested bytes from the disk (run 254:
// 8961 MiB for 2123 MiB). Reads of a block or more go straight to the file.
constexpr uint64_t kBlock = 64ull << 10;

// Persistent copy of what was read from slow storage (WSL's drvfs: ~4 ms per
// read, the frame of GTA VI's scene waits for its streaming): every block
// read from a file under /mnt/ is also written to a sparse file of the same
// size under XBEM_DISK_CACHE_DIR (default ~/.cache/xbem/files), and a bitmap
// next to it says which blocks are there. Later reads of those blocks - in
// this run or the next ones - come from the local disk. XBEM_DISK_CACHE=0
// turns it off; XBEM_DISK_CACHE_GB (default 120) caps what is added.
struct DiskCache {
  int fd = -1;               // the sparse copy
  uint8_t* bits = nullptr;   // 1 bit per block (mmap of the .bits file)
  uint64_t blocks = 0;
  bool has(uint64_t b) const { return b < blocks && (__atomic_load_n(&bits[b >> 3], __ATOMIC_ACQUIRE) >> (b & 7)) & 1; }
  void set(uint64_t b) {
    if (b < blocks) __atomic_fetch_or(&bits[b >> 3], static_cast<uint8_t>(1u << (b & 7)), __ATOMIC_RELEASE);
  }
};

struct OpenFile {
  int fd = -1;
  uint32_t id = 0;
  uint64_t size = 0;  // at open (game files do not change)
  DiskCache* disk = nullptr;
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
  std::atomic<uint64_t> local_bytes{0}, added_bytes{0};

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
int64_t source_read(const OpenFile&, uint64_t, void*, size_t) { return -1; }
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
    std::fprintf(stderr,
                 "[info ][files] %llu disk reads, %llu MiB, %.1f s reading; %llu requests for %llu MiB; local disk "
                 "cache: %llu MiB read from it, %llu MiB added\n",
                 static_cast<unsigned long long>(reads), static_cast<unsigned long long>(g_cache.disk_bytes >> 20),
                 g_cache.disk_ns / 1e9, static_cast<unsigned long long>(g_cache.requests),
                 static_cast<unsigned long long>(g_cache.requested >> 20),
                 static_cast<unsigned long long>(g_cache.local_bytes.load() >> 20),
                 static_cast<unsigned long long>(g_cache.added_bytes.load() >> 20));
  return r;
}
// Reads on WSL's drvfs (/mnt/<drive>) fail now and then (EIO, ENOMEM) or
// return 0 before the end of the file when memory is short: retried, with a
// short pause, before a failure is reported (a failed DirectStorage request
// made GTA VI place a resource over stale bytes and crash).
int64_t raw_read_uncounted(int fd, uint64_t offset, void* out, size_t n) {
  size_t done = 0;
  int retries = 0;
  while (done < n) {
    const ssize_t r = ::pread(fd, static_cast<uint8_t*>(out) + done, n - done, static_cast<off_t>(offset + done));
    if (r < 0 && errno == EINTR) continue;  // a signal (thread dumps, faults skipped) - not the end of the data
    if (r <= 0) {
      const int e = r < 0 ? errno : 0;
      struct stat st {};
      const bool before_end = ::fstat(fd, &st) == 0 && offset + done < static_cast<uint64_t>(st.st_size);
      if ((r < 0 || before_end) && retries < 12) {
        static std::atomic<int> warned{0};
        if (warned.fetch_add(1) < 16)
          std::fprintf(stderr, "[warn ][files] read of %zu bytes at %#llx %s (%s) - retrying\n", n - done,
                       static_cast<unsigned long long>(offset + done), r < 0 ? "failed" : "returned 0 before the end",
                       r < 0 ? std::strerror(e) : "short");
        ::usleep(2000u << std::min(retries, 6));
        ++retries;
        continue;
      }
      if (r < 0) return done ? static_cast<int64_t>(done) : -1;
      break;
    }
    done += static_cast<size_t>(r);
  }
  return static_cast<int64_t>(done);
}
int open_file(const std::string& path) { return ::open(path.c_str(), O_RDONLY | O_CLOEXEC); }

DiskCache* open_disk_cache(const std::string& path, int fd, uint64_t size) {
  static const bool on = [] {
    const char* v = std::getenv("XBEM_DISK_CACHE");
    return !(v && *v == '0');
  }();
  if (!on || path.rfind("/mnt/", 0) != 0 || size < kBlock) return nullptr;
  struct stat st {};
  if (::fstat(fd, &st) != 0) return nullptr;
  std::string dir;
  if (const char* d = std::getenv("XBEM_DISK_CACHE_DIR")) dir = d;
  else if (const char* h = std::getenv("HOME")) dir = std::string(h) + "/.cache/xbem/files";
  else return nullptr;
  for (size_t i = 1; i <= dir.size(); ++i)
    if (i == dir.size() || dir[i] == '/') ::mkdir(dir.substr(0, i).c_str(), 0755);
  // Named by the file and its size and modification time: a changed file
  // gets a new copy.
  uint64_t h = 1469598103934665603ull;
  const auto mix = [&](const void* p, size_t n) {
    for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const uint8_t*>(p)[i]) * 1099511628211ull;
  };
  mix(path.data(), path.size());
  mix(&size, sizeof size);
  const int64_t mtime = static_cast<int64_t>(st.st_mtime);
  mix(&mtime, sizeof mtime);
  std::string base = path.substr(path.find_last_of('/') + 1);
  char name[64];
  std::snprintf(name, sizeof name, "-%016llx", static_cast<unsigned long long>(h));
  const std::string stem = dir + "/" + base + name;
  auto* d = new DiskCache;
  d->blocks = (size + kBlock - 1) / kBlock;
  d->fd = ::open((stem + ".data").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  const int bits_fd = ::open((stem + ".bits").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  const uint64_t bits_size = (d->blocks + 7) / 8;
  bool ok = d->fd >= 0 && bits_fd >= 0 && ::ftruncate(d->fd, static_cast<off_t>(size)) == 0 &&
            ::ftruncate(bits_fd, static_cast<off_t>(bits_size)) == 0;
  if (ok) {
    void* m = ::mmap(nullptr, bits_size, PROT_READ | PROT_WRITE, MAP_SHARED, bits_fd, 0);
    ok = m != MAP_FAILED;
    if (ok) d->bits = static_cast<uint8_t*>(m);
  }
  if (bits_fd >= 0) ::close(bits_fd);
  if (!ok) {
    if (d->fd >= 0) ::close(d->fd);
    delete d;
    return nullptr;
  }
  return d;
}

uint64_t disk_cache_budget() {
  static const uint64_t b = [] {
    const char* v = std::getenv("XBEM_DISK_CACHE_GB");
    return (v ? std::strtoull(v, nullptr, 10) : 120ull) << 30;
  }();
  return b;
}

// Reads [offset, offset + n) of `f`: blocks in the local copy from it, the
// others from the file (whole blocks, then kept in the copy).
int64_t source_read(const OpenFile& f, uint64_t offset, void* out, size_t n) {
  if (!f.disk) return raw_read(f.fd, offset, out, n);
  DiskCache& d = *f.disk;
  if (offset >= f.size) return 0;
  n = static_cast<size_t>(std::min<uint64_t>(n, f.size - offset));
  if (!n) return 0;
  auto* o = static_cast<uint8_t*>(out);
  const uint64_t last = (offset + n - 1) / kBlock;
  for (uint64_t b = offset / kBlock; b <= last;) {
    const bool have = d.has(b);
    uint64_t e = b;
    while (e < last && d.has(e + 1) == have) ++e;
    const uint64_t lo = std::max(offset, b * kBlock), hi = std::min(offset + n, (e + 1) * kBlock);
    bool served = false;
    if (have) {
      served = raw_read_uncounted(d.fd, lo, o + (lo - offset), hi - lo) == static_cast<int64_t>(hi - lo);
      if (served) g_cache.local_bytes += hi - lo;
    }
    if (!served) {
      const uint64_t from = b * kBlock, to = std::min(f.size, (e + 1) * kBlock);
      std::vector<uint8_t> buf(to - from);
      const int64_t got = raw_read(f.fd, from, buf.data(), buf.size());
      if (got <= 0) return lo > offset ? static_cast<int64_t>(lo - offset) : -1;
      const uint64_t end = from + static_cast<uint64_t>(got);
      if (g_cache.added_bytes.load(std::memory_order_relaxed) < disk_cache_budget() &&
          ::pwrite(d.fd, buf.data(), static_cast<size_t>(got), static_cast<off_t>(from)) == got) {
        // Data first, then the bits: a block marked present is complete.
        for (uint64_t k = b; k <= e; ++k)
          if ((k + 1) * kBlock <= end || end == f.size) d.set(k);
        g_cache.added_bytes += static_cast<uint64_t>(got);
      }
      if (end < hi) {
        if (end > lo) std::memcpy(o + (lo - offset), buf.data() + (lo - from), end - lo);
        return static_cast<int64_t>(std::max(end, lo) - offset);
      }
      std::memcpy(o + (lo - offset), buf.data() + (lo - from), hi - lo);
    }
    b = e + 1;
  }
  return static_cast<int64_t>(n);
}
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
#if !defined(_WIN32)
      struct stat st {};
      if (::fstat(o.fd, &st) == 0) o.size = static_cast<uint64_t>(st.st_size);
      o.disk = open_disk_cache(host_path, o.fd, o.size);
#endif
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
  if (!c.budget || n >= kBlock) return source_read(f, offset, out, n);
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
      const int64_t got = source_read(f, block * kBlock, b->data.data(), kBlock);
      if (got < 0) return done ? static_cast<int64_t>(done) : -1;
      b->data.resize(static_cast<size_t>(got));
      // A short block is kept only at the end of the file: a short read in
      // the middle (a failed read) would otherwise end every later read there.
      const bool complete = got == static_cast<int64_t>(kBlock) || block * kBlock + got >= f.size;
      std::lock_guard lock(c.mutex);
      if (complete && c.index.find(key) == c.index.end()) {
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
