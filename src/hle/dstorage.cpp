// xcrdapi.dll: console DirectStorage (DStorageGetFactory).
//
// The console interfaces keep the method order of the public PC dstorage.h
// for the parts a title was seen using:
//
//   factory: 4 OpenFile(path, riid, ppv), 5 CreateStatusArray(capacity, name,
//            riid, ppv), 10 creates a queue (desc, riid, ppv); other slots
//            (e.g. 8 = staging buffer size) are accepted and logged.
//   file:    3 Close, 4 GetFileInformation(BY_HANDLE_FILE_INFORMATION*)
//   queue:   3 EnqueueRequest(request*), 4 EnqueueStatus(array, index),
//            5 EnqueueSignal(fence, value), 6 Submit, 8 Close,
//            12 EnqueueSetEvent(HANDLE) (set at once when it is an event)
//   status:  3 IsComplete(index), 4 GetHResult(index)
//
// Every queue has a worker thread that executes its work in order (file
// read, then decompression; status entries, fences and events after the
// requests before them), as the console's DirectStorage hardware does: the
// thread that enqueues and submits is not held up by the reads (doing them
// on the title's main thread stalled GTA VI's loading until its watchdog
// fired). XBEM_DSTORAGE_SYNC=1 runs the work on the enqueuing thread instead. The request structure is the console's: it is decoded by locating
// the file object among its fields (the emulator knows its file objects) -
// the offset and the compressed size follow it - and the destination buffer
// and size before it. The first requests are logged in full.

#include <atomic>
#include <condition_variable>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "common/inflate.h"
#include "common/strings.h"
#include "hle/d3d12_device.h"
#include "hle/d3d12_internal.h"
#include "hle/discovery.h"
#include "hle/heap.h"
#include "hle/file_log.h"
#include "hle/hle_util.h"
#include "hle/probe_com.h"
#include "hle/registry.h"
#include "platform/file_cache.h"
#include "platform/memory.h"
#include "runtime/objects.h"

namespace xbem::hle {
namespace {

using namespace win;

std::atomic<int> g_logged_requests{0};

// ------------------------------------------------------------------ files

struct FileState {
  std::mutex mutex;
  std::ifstream stream;
  std::string guest_path;
  uint64_t size = 0;

  // Which parts of the file requests have read (merged; gaps under 4 KiB,
  // such as the padding between zlib streams, count as read). The Submit
  // log lists the gaps, e.g. chunks of a file the title never asked for.
  std::mutex coverage_mutex;
  std::map<uint64_t, uint64_t> covered;  // start -> end
  uint64_t requests = 0, requests_logged = 0;
  // Where each request put its data (file offset -> destination, size): the
  // memory a gap's data belongs to lies between its neighbours' destinations.
  std::map<uint64_t, std::pair<uint64_t, uint64_t>> dest_by_offset;

  void add_coverage(uint64_t start, uint64_t length) {
    constexpr uint64_t kSlack = 4096;
    std::lock_guard lock(coverage_mutex);
    ++requests;
    uint64_t end = start + length;
    auto it = covered.upper_bound(start);
    if (it != covered.begin() && std::prev(it)->second + kSlack >= start) --it;
    while (it != covered.end() && it->first <= end + kSlack) {
      start = std::min(start, it->first);
      end = std::max(end, it->second);
      it = covered.erase(it);
    }
    covered[start] = end;
  }

  std::string host_path;
  bool read(uint64_t offset, uint8_t* out, size_t n) {
    return platform::cached_read(host_path, offset, out, n) == static_cast<int64_t>(n);
  }
};

std::mutex g_files_mutex;
std::set<const void*> g_files;

bool is_file(uint64_t p) {
  std::lock_guard lock(g_files_mutex);
  return g_files.count(reinterpret_cast<const void*>(p)) != 0;
}

FileState* file_state(uint64_t p) { return static_cast<FileState*>(reinterpret_cast<ProbeObject*>(p)->state); }

HRESULT XBEM_GUEST_ABI file_close(ProbeObject* self) {
  auto* f = static_cast<FileState*>(self->state);
  std::lock_guard lock(f->mutex);
  f->stream.close();
  return S_OK;
}

// BY_HANDLE_FILE_INFORMATION: attributes @0, times @4..27, volume @28,
// size high @32, size low @36, links @40, index @44/48 (52 bytes).
HRESULT XBEM_GUEST_ABI file_get_information(ProbeObject* self, uint8_t* info) {
  auto* f = static_cast<FileState*>(self->state);
  if (!info) return E_POINTER;
  uint8_t b[52] = {};
  const uint32_t attributes = 0x80 /*NORMAL*/ | 0x1 /*READONLY*/, high = f->size >> 32,
                 low = static_cast<uint32_t>(f->size), links = 1;
  std::memcpy(b + 0, &attributes, 4);
  std::memcpy(b + 32, &high, 4);
  std::memcpy(b + 36, &low, 4);
  std::memcpy(b + 40, &links, 4);
  std::memcpy(info, b, sizeof b);
  return S_OK;
}

const ProbeVtable& file_vtable() {
  static const ProbeVtable v = make_probe_vtable(
      {{3, reinterpret_cast<void*>(&file_close)}, {4, reinterpret_cast<void*>(&file_get_information)}});
  return v;
}

// ---------------------------------------------------------- status arrays

struct StatusState {
  std::mutex mutex;
  std::vector<uint8_t> complete;
  std::vector<HRESULT> result;
  // Status entries enqueued and not reached yet, per index: an entry is
  // complete only when the queue reached the last one (GTA VI reuses a
  // one-entry array; the queue reaching the previous batch's entry after the
  // next one was enqueued marked the new batch complete before its reads).
  std::vector<uint32_t> pending;
};

BOOL XBEM_GUEST_ABI status_is_complete(ProbeObject* self, uint32_t index) {
  auto* s = static_cast<StatusState*>(self->state);
  std::lock_guard lock(s->mutex);
  return index < s->complete.size() && s->complete[index];
}
HRESULT XBEM_GUEST_ABI status_get_hresult(ProbeObject* self, uint32_t index) {
  auto* s = static_cast<StatusState*>(self->state);
  std::lock_guard lock(s->mutex);
  return index < s->result.size() ? s->result[index] : E_INVALIDARG;
}

const ProbeVtable& status_vtable() {
  static const ProbeVtable v = make_probe_vtable(
      {{3, reinterpret_cast<void*>(&status_is_complete)}, {4, reinterpret_cast<void*>(&status_get_hresult)}});
  return v;
}

bool is_status_array(uint64_t p) {
  uint64_t vt = 0;
  return p >= 0x10000 && platform::safe_read(p, &vt, 8) &&
         vt == reinterpret_cast<uint64_t>(status_vtable().data());
}

// ------------------------------------------------------------------ queues

struct QueueState {
  std::mutex mutex;
  HRESULT first_error = S_OK;  // since the last status/signal
  uint64_t requests = 0, bytes_read = 0, bytes_out = 0;
  // Statistics for the Submit log: requests per compression format, failed
  // requests, and requests whose source is memory (not executed yet).
  std::map<uint32_t, uint64_t> by_compression;
  uint64_t failed = 0, memory_sources = 0;
  // Enqueued work, executed in order by Submit (as the real queue does: a
  // title may fill memory a request refers to between Enqueue and Submit).
  struct Pending {
    enum Kind { Request, Status, Signal, Event } kind;
    uint64_t words[12];  // a request (copied at Enqueue)
    uint64_t object;     // status array / fence / event handle
    uint64_t value;      // status index / fence value
    uint64_t caller;
  };
  std::vector<Pending> pending;  // waiting for Submit
  // Released work, run in order by the worker thread.
  std::vector<Pending> ready;
  std::condition_variable wake;
  bool worker_started = false;
};

// Every queue and status array, for the status report.
std::mutex g_queues_mutex;
std::vector<QueueState*> g_queues;
std::vector<StatusState*> g_status_arrays;

std::string hex_qwords(const uint64_t* q, int n) {
  std::string s;
  for (int i = 0; i < n; ++i) s += std::format(" {:016x}", q[i]);
  return s;
}

// Executes one request; returns its HRESULT.
// Requests into the middle of a compressed chunk. GTA VI's read layer (fn
// 0x4ffd520) maps a texture's pieces onto the .ttpf chunk table; when a
// piece does not start at a chunk (its own int3 at +0x4ffd868 - our tiling
// answers cut textures into smaller pieces than the files were chunked for)
// it still enqueues the chunk's compressed size from "chunk start + offset of
// the piece in the chunk", which is not the start of a DEFLATE stream. The
// piece's data is bytes [shift, shift + size) of the chunk that starts
// `shift` bytes earlier: find that start (4 KiB steps back, the stream must
// decode completely) and keep the last chunks decoded, since the pieces of
// one chunk arrive one after another.
struct DecodedChunk {
  const FileState* file = nullptr;
  uint64_t start = 0, compressed = 0;
  std::vector<uint8_t> data;
};
std::mutex g_chunk_mutex;
std::vector<DecodedChunk> g_chunks;  // most recent last
std::map<std::pair<const FileState*, uint64_t>, uint64_t> g_chunk_start;  // (file, bad offset) -> chunk start
std::atomic<uint64_t> g_shifted_ok{0}, g_shifted_failed{0};
// Offsets where compressed requests decoded from their first byte: chunk
// starts, tried first for a piece inside a chunk.
std::map<const FileState*, std::set<uint64_t>> g_chunk_starts;

void note_chunk_start(const FileState* file, uint64_t offset) {
  std::lock_guard lock(g_chunk_mutex);
  g_chunk_starts[file].insert(offset);
}

constexpr uint64_t kMaxChunkShift = 0x40000, kMaxChunkBytes = 0x100000;

const DecodedChunk* decoded_chunk(FileState* file, uint64_t start, uint64_t compressed) {
  for (const DecodedChunk& c : g_chunks)
    if (c.file == file && c.start == start && c.compressed == compressed) return &c;
  std::vector<uint8_t> in(compressed);
  if (!file->read(start, in.data(), in.size())) return nullptr;
  DecodedChunk c{file, start, compressed, std::vector<uint8_t>(kMaxChunkBytes)};
  size_t produced = 0;
  if (inflate_auto(in.data(), in.size(), c.data.data(), c.data.size(), produced) != InflateResult::Ok) return nullptr;
  c.data.resize(produced);
  if (g_chunks.size() >= 8) g_chunks.erase(g_chunks.begin());
  g_chunks.push_back(std::move(c));
  return &g_chunks.back();
}

// Bytes [skip, skip + size) of the piece at `offset` (compressed size
// `compressed`, the whole chunk's) into out[skip...]; false when no chunk
// start before `offset` decodes.
bool shifted_chunk_piece(FileState* file, uint64_t offset, uint64_t compressed, uint64_t skip, uint64_t size,
                         std::vector<uint8_t>& out, size_t& produced) {
  // Off by default: with it GTA VI's story load never left the loading
  // screen (reads stopped at ~240 s in every run), without it - the pieces
  // failing - the title reaches the scene after it (2026-11-27, runs 12/13).
  static const bool enabled = [] {
    const char* v = std::getenv("XBEM_CHUNK_RECOVERY");
    return v && *v && *v != '0';
  }();
  if (!enabled) return false;
  std::lock_guard lock(g_chunk_mutex);
  const auto key = std::make_pair(static_cast<const FileState*>(file), offset);
  uint64_t start = UINT64_MAX;
  if (auto it = g_chunk_start.find(key); it != g_chunk_start.end()) {
    if (it->second == UINT64_MAX - 1) return false;  // searched before, nothing found
    start = it->second;
  }
  const DecodedChunk* c = nullptr;
  if (start == UINT64_MAX) {
    // The nearest chunk start seen before.
    if (auto f = g_chunk_starts.find(file); f != g_chunk_starts.end()) {
      auto it = f->second.lower_bound(offset);
      if (it != f->second.begin() && offset - *std::prev(it) <= kMaxChunkShift) {
        const uint64_t candidate = *std::prev(it);
        c = decoded_chunk(file, candidate, compressed);
        // (The title asks for the chunk's full size even from inside it: the
        // piece gets what the chunk still holds.)
        if (c && c->data.size() > offset - candidate + skip) g_chunk_start[key] = start = candidate;
        else c = nullptr;
      }
    }
  }
  if (c) {
  } else if (start != UINT64_MAX) {
    c = decoded_chunk(file, start, compressed);
  } else {
    // Chunk starts are 256-byte aligned (the chunk table stores offsets >>
    // 8). One read of the window before the piece; only candidates with a
    // zlib header (CMF method 8, (CMF << 8 | FLG) % 31 == 0) are inflated.
    const uint64_t back = std::min<uint64_t>(kMaxChunkShift, offset & ~uint64_t{0xff});
    std::vector<uint8_t> window(back + 2);
    if (back && file->read(offset - back, window.data(), window.size())) {
      for (uint64_t shift = 0x100; shift <= back && !c; shift += 0x100) {
        const uint8_t* h = window.data() + (back - shift);
        if ((h[0] & 0x0f) != 8 || ((h[0] << 8) | h[1]) % 31 != 0) continue;
        c = decoded_chunk(file, offset - shift, compressed);
        if (c && c->data.size() <= shift + skip) c = nullptr;  // decodes, but ends before the piece
        if (c) g_chunk_start[key] = start = offset - shift;
      }
    }
    // Raw DEFLATE chunks have no header to test: 4 KiB steps, as pieces are
    // page-aligned there.
    for (uint64_t shift = 0x1000; shift <= back && !c; shift += 0x1000) {
      const uint8_t* h = window.data() + (back - shift);
      if ((h[0] & 0x0f) == 8 && ((h[0] << 8) | h[1]) % 31 == 0) continue;  // tried above
      c = decoded_chunk(file, offset - shift, compressed);
      if (c && c->data.size() <= shift + skip) c = nullptr;
      if (c) g_chunk_start[key] = start = offset - shift;
    }
    // Remember failures too: the title retries such requests.
    if (!c) g_chunk_start[key] = UINT64_MAX - 1;
  }
  if (!c) {
    if (g_shifted_failed.fetch_add(1) < 4)
      XBEM_WARN("dstorage", "{}: no compressed chunk before {:#x} holds the piece", file->guest_path, offset);
    return false;
  }
  const uint64_t from = offset - start + skip;
  produced = from < c->data.size() ? std::min<uint64_t>(size, c->data.size() - from) : 0;
  out.assign(skip + size, 0);
  std::memcpy(out.data() + skip, c->data.data() + from, produced);
  produced += skip;
  if (g_shifted_ok.fetch_add(1) < 4)
    XBEM_INFO("dstorage", "{}: piece at {:#x} is {:#x} bytes into the compressed chunk at {:#x} - delivered from it",
              file->guest_path, offset, offset - start, start);
  return true;
}

HRESULT execute_request(QueueState* q, const uint8_t* request, uint64_t caller) {
  uint64_t w[12] = {};
  if (!request || !platform::safe_read(reinterpret_cast<uint64_t>(request), w, sizeof w)) return E_POINTER;
  const bool log = g_logged_requests.fetch_add(1) < 6;
  if (log) XBEM_INFO("dstorage", "EnqueueRequest from {}:{}", describe_code_address(caller), hex_qwords(w, 12));
  if ((w[0] & 0xff0000) && std::getenv("XBEM_DSTORAGE_TRACE")) {
    static std::atomic<int> n{0};
    if (n.fetch_add(1) < 60) {
      uint64_t at1[6] = {}, at10[6] = {}, at11[6] = {};
      platform::safe_read(w[1], at1, sizeof at1);
      platform::safe_read(w[10], at10, sizeof at10);
      platform::safe_read(w[11], at11, sizeof at11);
      XBEM_INFO("dstorage", "request{}\n  [w1]{}\n  [w10]{}\n  [w11]{}", hex_qwords(w, 12), hex_qwords(at1, 6),
                hex_qwords(at10, 6), hex_qwords(at11, 6));
    }
  }

  // Locate the source file among the fields.
  int fi = -1;
  for (int i = 1; i < 11 && fi < 0; ++i)
    if (is_file(w[i])) fi = i;
  if (fi < 0) {
    static std::atomic<int> warned{0};
    if (warned.fetch_add(1) < 4)
      XBEM_WARN("dstorage", "request without a known file (memory source?) - not executed:{}", hex_qwords(w, 12));
    return E_NOTIMPL;
  }
  FileState* file = file_state(w[fi]);
  const uint64_t offset = w[fi + 1];
  const uint32_t source_size = static_cast<uint32_t>(w[fi + 2]);
  file->add_coverage(offset, source_size);
  const uint32_t uncompressed_size = static_cast<uint32_t>(w[fi + 2] >> 32);
  const uint32_t compression = static_cast<uint32_t>(w[0] & 0xFF);
  {
    std::lock_guard lock(q->mutex);
    ++q->by_compression[compression];
  }
  // Destination memory: the first pointer before the file, its size the next
  // non-zero word.
  uint8_t* dest = nullptr;
  uint64_t dest_size = 0;
  // A buffer destination: (ID3D12Resource*, offset, size) - the resource is
  // one of the emulator's objects, its memory the destination.
  if (fi >= 4) {
    if (d3d12::ResourceState* res = d3d12::resource_of(w[1]); res && res->gpu_va) {
      dest = reinterpret_cast<uint8_t*>(res->gpu_va + w[2]);
      dest_size = static_cast<uint32_t>(w[3]);
    }
  }
  // The usual layout: destination (base, offset, size) before the file. The
  // source chunk covers the region starting at `base`; a request delivers
  // bytes [offset, offset + size) of the decompressed chunk to base + offset
  // (GTA VI's .ttpf streaming fills a region page by page this way: one base,
  // offsets 0, 0x1000, 0x2000, ... each from a 16 KiB chunk).
  uint64_t skip = 0;
  // Options 0x20xxxx: the destination is physical memory - word 1 points to
  // the frame numbers of its 64 KiB pages (one 8-byte entry per page, as
  // XMemAllocatePhysicalPages hands them out), word 2 is the byte offset in
  // that destination and word 3 the size. The source chunk covers the
  // destination from its start: bytes [offset, offset + size) of the
  // decompressed chunk are delivered. (GTA VI streams its .ttpf data so;
  // treating word 1 as plain memory overwrote the title's page arrays.)
  const bool physical = (w[0] & 0x200000) && fi == 4 && w[1] >= 0x10000 && w[3] && w[3] < (1ull << 32);
  if (physical) {
    skip = w[2] < (1ull << 32) ? w[2] : 0;
    dest = reinterpret_cast<uint8_t*>(w[1]);  // the frame number array
    dest_size = w[3];
  } else if (!dest && fi == 4 && w[1] >= 0x10000 && w[1] < 0x800000000000ull && w[3] && w[3] < (1ull << 32)) {
    skip = w[2] < (1ull << 32) ? w[2] : 0;
    dest = reinterpret_cast<uint8_t*>(w[1] + skip);
    dest_size = w[3];
  }
  for (int i = 1; i < fi && !dest_size; ++i) {
    if (!dest && w[i] >= 0x10000 && w[i] < 0x800000000000ull) {
      dest = reinterpret_cast<uint8_t*>(w[i]);
    } else if (dest && !dest_size && w[i] && w[i] < (1ull << 32)) {
      dest_size = w[i];
    }
  }
  if (!dest_size) dest_size = uncompressed_size ? uncompressed_size : source_size;
  {
    std::lock_guard lock(file->coverage_mutex);
    file->dest_by_offset[offset] = {reinterpret_cast<uint64_t>(dest), dest_size};
  }
  if (file_reads_logged(file->guest_path) && take_dstorage_log_slot())
    XBEM_INFO("read", "{} DirectStorage at {:#x} size {:#x} (uncompressed {:#x}, compression {}) into {} size {:#x} from {}",
              file->guest_path, offset, source_size, uncompressed_size, compression, static_cast<void*>(dest),
              dest_size, describe_code_address(caller));
  if (log)
    XBEM_INFO("dstorage",
              "  -> {} offset {:#x} size {:#x} (uncompressed {:#x}), compression {}, dest {} size {:#x}",
              file->guest_path, offset, source_size, uncompressed_size, compression, static_cast<void*>(dest),
              dest_size);
  if (!dest || !source_size || source_size > (256u << 20) || skip + dest_size > (1ull << 30)) return E_INVALIDARG;

  std::vector<uint8_t> buffer(source_size);
  if (!file->read(offset, buffer.data(), source_size)) {
    XBEM_WARN("dstorage", "{}: read of {:#x} bytes at {:#x} failed (file size {:#x})", file->guest_path, source_size,
              offset, file->size);
    return HRESULT(0x80070026);  // HRESULT_FROM_WIN32(ERROR_HANDLE_EOF)
  }
  std::lock_guard lock(q->mutex);
  ++q->requests;
  q->bytes_read += source_size;
  // Writes [from, from + n) of the destination's bytes into its physical pages.
  const auto to_physical = [&](const uint8_t* data, uint64_t from, uint64_t n) {
    bool ok = true;
    while (n) {
      const uint64_t page = from >> 16, in_page = from & 0xffff, chunk = std::min<uint64_t>(n, 0x10000 - in_page);
      uint64_t pfn = 0;
      ok &= platform::safe_read(w[1] + page * 8, &pfn, 8) && write_physical_page(pfn, in_page, data, chunk);
      data += chunk;
      from += chunk;
      n -= chunk;
    }
    return ok;
  };
  if (physical) {
    std::vector<uint8_t> whole;
    size_t produced = 0;
    InflateResult r = InflateResult::Ok;
    if (compression == 0) {
      whole = buffer;
      produced = whole.size();
    } else {
      whole.resize(skip + dest_size);
      r = inflate_auto(buffer.data(), buffer.size(), whole.data(), whole.size(), produced);
      if (r == InflateResult::OutputFull) r = InflateResult::Ok;
      if (r == InflateResult::Ok && compression == 1) note_chunk_start(file, offset);
      if (r != InflateResult::Ok && compression == 1 &&
          shifted_chunk_piece(file, offset, source_size, skip, dest_size, whole, produced))
        r = InflateResult::Ok;
    }
    if (r == InflateResult::Ok) {
      // An uncompressed source shorter than the window holds just the window.
      const uint64_t start = produced >= skip + dest_size ? skip : 0;
      const uint64_t n = std::min<uint64_t>(dest_size, produced > start ? produced - start : 0);
      if (!to_physical(whole.data() + start, skip, n)) {
        static std::atomic<int> warned{0};
        if (warned.fetch_add(1) < 8)
          XBEM_WARN("dstorage", "{}: physical destination with unknown frames:{}", file->guest_path, hex_qwords(w, 12));
      }
      q->bytes_out += n;
      return S_OK;
    }
    // An undecodable format: reported (and failed) below.
  }
  // Into guest memory with safe writes: the worker runs while the title goes
  // on, and a destination that is not mapped (any more) must fail the
  // request, not the emulator.
  const auto deliver = [&](const uint8_t* data, uint64_t n) {
    if (!n || platform::safe_write(reinterpret_cast<uint64_t>(dest), data, n)) return true;
    static std::atomic<int> warned{0};
    if (warned.fetch_add(1) < 16)
      XBEM_WARN("dstorage", "{}: destination {} ({:#x} bytes) is not writable - request failed:{}", file->guest_path,
                static_cast<void*>(dest), n, hex_qwords(w, 12));
    return false;
  };
  if (compression == 0) {
    const uint64_t n = std::min<uint64_t>(source_size, dest_size);
    if (!deliver(buffer.data(), n)) return HRESULT(0x800706F8);  // HRESULT_FROM_WIN32(ERROR_INVALID_USER_BUFFER)
    q->bytes_out += n;
    return S_OK;
  }
  size_t produced = 0;
  InflateResult r;
  {
    std::vector<uint8_t> whole(skip + dest_size);
    r = inflate_auto(buffer.data(), buffer.size(), whole.data(), whole.size(), produced);
    // A piece shorter than its chunk (the title's int3 at +0x4ffd879): the
    // rest of the chunk is not wanted.
    if (r == InflateResult::OutputFull) r = InflateResult::Ok;
    if (r == InflateResult::Ok && compression == 1) note_chunk_start(file, offset);
    if ((r == InflateResult::BadData || r == InflateResult::Truncated) && compression == 1 &&
        shifted_chunk_piece(file, offset, source_size, skip, dest_size, whole, produced))
      r = InflateResult::Ok;
    // (A physical destination only gets here with an undecodable format: its
    // `dest` is the frame number array, which is never written.)
    if (!physical) {
      const uint64_t n = produced > skip ? std::min<uint64_t>(produced - skip, dest_size) : 0;
      if (!deliver(whole.data() + skip, n)) return HRESULT(0x800706F8);
    }
  }
  q->bytes_out += produced;
  if (r != InflateResult::Ok) {
    static std::atomic<int> warned{0};
    if (warned.fetch_add(1) < 8)
      XBEM_WARN("dstorage", "{}: compression {} chunk at {:#x} ({:#x} bytes): inflate failed ({}) after {:#x} bytes; "
                "first bytes {:02x} {:02x} {:02x} {:02x}; request:{}",
                file->guest_path, compression, offset, source_size, inflate_result_name(r), produced, buffer[0],
                buffer.size() > 1 ? buffer[1] : 0, buffer.size() > 2 ? buffer[2] : 0, buffer.size() > 3 ? buffer[3] : 0,
                hex_qwords(w, 12));
    if (warned.load() <= 8) {
      uint64_t at_w1[6] = {};
      platform::safe_read(w[1], at_w1, sizeof at_w1);
      XBEM_WARN("dstorage", "  memory at word 1:{}", hex_qwords(at_w1, 6));
    }
    // Formats the emulator cannot decode (2 and 14 in GTA VI: no public
    // description) are delivered as zeros and reported as done. Leaving the
    // destination alone kept stale contents of recycled physical pages, which
    // the title then followed as pointers (crashes in 2 of 3 runs); zeros it
    // treats as empty. XBEM_DSTORAGE_STRICT=1 fails these requests instead.
    static const bool strict = [] {
      const char* v = std::getenv("XBEM_DSTORAGE_STRICT");
      return v && *v && *v != '0';
    }();
    if (!strict && compression != 1) {
      const std::vector<uint8_t> zeros(dest_size);
      if (physical) to_physical(zeros.data(), skip, zeros.size());
      else deliver(zeros.data(), zeros.size());
      return S_OK;
    }
    return HRESULT(0x80070017);  // HRESULT_FROM_WIN32(ERROR_CRC): corrupt data
  }
  return S_OK;
}

// For the Submit log: files with new requests since the last log, the range
// the requests cover and the gaps inside it.
void log_file_coverage() {
  std::vector<FileState*> files;
  {
    std::lock_guard lock(g_files_mutex);
    for (const void* f : g_files) files.push_back(file_state(reinterpret_cast<uint64_t>(f)));
  }
  for (FileState* f : files) {
    std::lock_guard lock(f->coverage_mutex);
    if (f->requests == f->requests_logged || f->requests < 16 || f->covered.empty()) continue;
    f->requests_logged = f->requests;
    std::string gaps;
    uint64_t gap_bytes = 0;
    int listed = 0;
    for (auto it = f->covered.begin(); std::next(it) != f->covered.end(); ++it) {
      const uint64_t from = it->second, to = std::next(it)->first;
      gap_bytes += to - from;
      if (listed++ < 8) gaps += std::format(" {:#x}-{:#x}", from, to);
    }
    XBEM_INFO("dstorage", "  {}: {} requests cover {:#x}-{:#x} of {:#x} bytes; {} gaps ({:#x} bytes){}{}",
              f->guest_path, f->requests, f->covered.begin()->first, f->covered.rbegin()->second, f->size,
              f->covered.size() - 1, gap_bytes, gaps.empty() ? "" : ":", gaps);
    // Did the data of each gap reach memory some other way (e.g. ReadFile)?
    // Its destination lies between the neighbouring requests' destinations,
    // and before the first request's when the data is laid out in order.
    listed = 0;
    for (auto it = f->covered.begin(); it != f->covered.end() && listed < 8; ++it) {
      const bool before_first = it == f->covered.begin();
      if (!before_first) ++listed;
      const uint64_t gap_end = it->first;
      const auto next = f->dest_by_offset.lower_bound(gap_end);
      if (next == f->dest_by_offset.end()) continue;
      uint64_t mem_start, mem_end = next->second.first;
      if (before_first) {
        mem_start = mem_end >= 0x10000 ? mem_end - 0x10000 : 0;  // one 64 KiB chunk before the first
      } else {
        const auto prev = std::prev(f->dest_by_offset.lower_bound(std::prev(it)->second));
        mem_start = prev->second.first + prev->second.second;
      }
      if (mem_end <= mem_start || mem_end - mem_start > (16u << 20)) continue;
      uint64_t nonzero = 0, first_nonzero = 0;
      bool readable = true;
      for (uint64_t a = mem_start; a < mem_end && readable; a += 4096) {
        uint8_t page[4096];
        const size_t n = static_cast<size_t>(std::min<uint64_t>(4096, mem_end - a));
        readable = platform::safe_read(a, page, n);
        for (size_t k = 0; readable && k < n; ++k)
          if (page[k] && !nonzero++) first_nonzero = a + k;
      }
      XBEM_INFO("dstorage", "    {} {:#x}: memory {:#x}-{:#x} ({:#x} bytes) {}",
                before_first ? "before the first request, at" : "gap ending at", gap_end, mem_start, mem_end,
                mem_end - mem_start,
                !readable ? std::string("is not readable")
                : nonzero ? std::format("has {:#x} non-zero bytes (first at {:#x})", nonzero, first_nonzero)
                          : std::string("is all zero"));
    }
  }
}

void run_request(QueueState* q, const uint8_t* request, uint64_t caller) {
  const HRESULT hr = execute_request(q, request, caller);
  if (hr < 0) {
    std::lock_guard lock(q->mutex);
    if (q->first_error >= 0) q->first_error = hr;
    if (hr == E_NOTIMPL) ++q->memory_sources;
    else ++q->failed;
  }
}

void run_status(QueueState* q, ProbeObject* array, uint32_t index) {
  if (!is_status_array(reinterpret_cast<uint64_t>(array))) return;
  auto* s = static_cast<StatusState*>(array->state);
  HRESULT hr;
  {
    std::lock_guard lock(q->mutex);
    hr = q->first_error;
    q->first_error = S_OK;
  }
  std::lock_guard lock(s->mutex);
  if (index < s->complete.size()) {
    if (s->pending[index]) --s->pending[index];
    if (!s->pending[index]) s->complete[index] = 1;
    s->result[index] = hr;
  }
}

void drain(QueueState* q);
void release(QueueState* q, std::vector<QueueState::Pending> work);

bool synchronous_queues() {
  static const bool sync = [] {
    const char* v = std::getenv("XBEM_DSTORAGE_SYNC");
    return v && *v && *v != '0';
  }();
  return sync;
}

// Work runs when it is enqueued, except requests into physical memory
// (options 0x20xxxx): the title fills their frame arrays after Enqueue and
// before Submit, so they wait for Submit - and so does everything enqueued
// after them, to keep the order. (Waiting for Submit with everything made
// GTA VI's synchronous reads hang: it relies on the queue starting work by
// itself.)
void enqueue(QueueState* q, QueueState::Pending p) {
  const bool deferred = p.kind == QueueState::Pending::Request && (p.words[0] & 0x200000);
  {
    std::lock_guard lock(q->mutex);
    if (deferred || !q->pending.empty()) {
      q->pending.push_back(p);
      return;
    }
  }
  release(q, {p});
}

HRESULT XBEM_GUEST_ABI queue_enqueue_request(ProbeObject* self, const uint8_t* request) {
  auto* q = static_cast<QueueState*>(self->state);
  QueueState::Pending p{QueueState::Pending::Request, {}, 0, 0, XBEM_RETURN_ADDRESS()};
  if (!request || !platform::safe_read(reinterpret_cast<uint64_t>(request), p.words, sizeof p.words)) return S_OK;
  enqueue(q, p);
  return S_OK;  // EnqueueRequest itself returns void on the PC; errors surface through status arrays
}

void XBEM_GUEST_ABI queue_enqueue_status(ProbeObject* self, ProbeObject* array, uint32_t index) {
  auto* q = static_cast<QueueState*>(self->state);
  if (!is_status_array(reinterpret_cast<uint64_t>(array))) return;
  {
    // The entry is incomplete until the queue gets there.
    auto* s = static_cast<StatusState*>(array->state);
    std::lock_guard lock(s->mutex);
    if (index < s->complete.size()) {
      s->complete[index] = 0;
      ++s->pending[index];
    }
  }
  enqueue(q, {QueueState::Pending::Status, {}, reinterpret_cast<uint64_t>(array), index, 0});
}

void XBEM_GUEST_ABI queue_enqueue_signal(ProbeObject* self, void* fence, uint64_t value) {
  enqueue(static_cast<QueueState*>(self->state),
          {QueueState::Pending::Signal, {}, reinterpret_cast<uint64_t>(fence), value, 0});
}

// 12 EnqueueSetEvent(HANDLE) in the PC interface: the event is set when the
// requests before it complete (at Submit). A handle that is no event (the
// console slot meaning something else) is only logged.
HRESULT XBEM_GUEST_ABI queue_enqueue_set_event(ProbeObject* self, uint64_t handle) {
  auto event = proc().handles().get_as<rt::EventObject>(handle);
  if (event) enqueue(static_cast<QueueState*>(self->state), {QueueState::Pending::Event, {}, handle, 0, 0});
  probe_log(self, 12,
            std::format("EnqueueSetEvent({:#x}) -> {}", handle, event ? "set when the work before it completes" : "not an event"),
            XBEM_RETURN_ADDRESS());
  return S_OK;
}

void run_work(QueueState* q, const std::vector<QueueState::Pending>& work) {
  for (const QueueState::Pending& p : work) {
    switch (p.kind) {
      case QueueState::Pending::Request:
        run_request(q, reinterpret_cast<const uint8_t*>(p.words), p.caller);
        break;
      case QueueState::Pending::Status:
        run_status(q, reinterpret_cast<ProbeObject*>(p.object), static_cast<uint32_t>(p.value));
        break;
      case QueueState::Pending::Signal:
        signal_d3d12_fence(reinterpret_cast<void*>(p.object), p.value);
        break;
      case QueueState::Pending::Event:
        if (auto event = proc().handles().get_as<rt::EventObject>(p.object)) event->set();
        break;
    }
  }
}

void worker(QueueState* q) {
  for (;;) {
    std::vector<QueueState::Pending> work;
    {
      std::unique_lock lock(q->mutex);
      q->wake.wait(lock, [q] { return !q->ready.empty(); });
      work.swap(q->ready);
    }
    run_work(q, work);
  }
}

// Hands work to the queue's worker (or runs it here in synchronous mode).
void release(QueueState* q, std::vector<QueueState::Pending> work) {
  if (work.empty()) return;
  if (synchronous_queues()) {
    run_work(q, work);
    return;
  }
  {
    std::lock_guard lock(q->mutex);
    q->ready.insert(q->ready.end(), work.begin(), work.end());
    if (!q->worker_started) {
      q->worker_started = true;
      std::thread(worker, q).detach();
    }
  }
  q->wake.notify_one();
}

// Releases everything waiting for Submit, in order.
void drain(QueueState* q) {
  std::vector<QueueState::Pending> work;
  {
    std::lock_guard lock(q->mutex);
    work.swap(q->pending);
  }
  release(q, std::move(work));
}

void XBEM_GUEST_ABI queue_submit(ProbeObject* self) {
  auto* q = static_cast<QueueState*>(self->state);
  drain(q);
  static std::atomic<int> logged{0};
  const int n = logged.fetch_add(1);
  if (n < 64 || (n & (n - 1)) == 0 || n % 256 == 0) {
    std::lock_guard lock(q->mutex);
    std::string formats;
    for (const auto& [format, count] : q->by_compression) formats += std::format(" {}:{}", format, count);
    XBEM_INFO("dstorage",
              "Submit #{}: {} requests so far, {:#x} bytes read, {:#x} bytes delivered; by compression format{}; "
              "failed {}, memory sources not executed {}",
              n + 1, q->requests, q->bytes_read, q->bytes_out, formats, q->failed, q->memory_sources);
    log_file_coverage();
  }
}

const ProbeVtable& queue_vtable() {
  static const ProbeVtable v = make_probe_vtable({{3, reinterpret_cast<void*>(&queue_enqueue_request)},
                                                  {4, reinterpret_cast<void*>(&queue_enqueue_status)},
                                                  {5, reinterpret_cast<void*>(&queue_enqueue_signal)},
                                                  {6, reinterpret_cast<void*>(&queue_submit)},
                                                  {12, reinterpret_cast<void*>(&queue_enqueue_set_event)}});
  return v;
}

// ---------------------------------------------------------------- factory

HRESULT out_object(void** out, void* object) {
  if (!out) return E_POINTER;
  *out = object;
  return S_OK;
}

// 4 OpenFile(path, riid, ppv)
HRESULT XBEM_GUEST_ABI factory_open_file(ProbeObject* self, const void* path, const void*, void** out) {
  if (!path) return E_INVALIDARG;
  // PCWSTR on the PC; accept a narrow string too.
  const auto* bytes = static_cast<const uint8_t*>(path);
  const std::string guest =
      bytes[1] == 0 ? narrow(static_cast<const char16_t*>(path)) : narrow(static_cast<const char*>(path));
  auto r = proc().vfs().resolve(std::string_view(guest));
  auto* state = new FileState;
  state->guest_path = guest;
  if (r) {
    state->stream.open(r->host_path, std::ios::binary);
    state->host_path = r->host_path.string();
  }
  static std::atomic<int> logged{0};
  if (!state->stream.is_open()) {
    delete state;
    XBEM_WARN("dstorage", "OpenFile({}) -> not found", guest);
    if (out) *out = nullptr;
    return HRESULT(0x80070002);  // HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
  }
  state->stream.seekg(0, std::ios::end);
  state->size = static_cast<uint64_t>(state->stream.tellg());
  void* object = make_probe_object("dstorage.file", &file_vtable(), state);
  {
    std::lock_guard lock(g_files_mutex);
    g_files.insert(object);
  }
  if (logged.fetch_add(1) < 24)
    probe_log(self, 4, std::format("OpenFile({}) -> {} bytes", guest, state->size), XBEM_RETURN_ADDRESS());
  return out_object(out, object);
}

// 5 CreateStatusArray(capacity, name, riid, ppv)
HRESULT XBEM_GUEST_ABI factory_create_status_array(ProbeObject* self, uint32_t capacity, const char*, const void*,
                                                   void** out) {
  auto* state = new StatusState;
  {
    std::lock_guard lock(g_queues_mutex);
    g_status_arrays.push_back(state);
  }
  state->complete.assign(std::min<uint32_t>(capacity, 1 << 20), 0);
  state->result.assign(state->complete.size(), S_OK);
  state->pending.assign(state->complete.size(), 0);
  probe_log(self, 5, std::format("CreateStatusArray({})", capacity), XBEM_RETURN_ADDRESS());
  return out_object(out, make_probe_object("dstorage.status_array", &status_vtable(), state));
}

// 10 (console): queue creation (desc, riid, ppv)
HRESULT XBEM_GUEST_ABI factory_create_queue(ProbeObject* self, const uint8_t* desc, const void*, void** out) {
  uint64_t d[4] = {};
  if (desc) platform::safe_read(reinterpret_cast<uint64_t>(desc), d, sizeof d);
  probe_log(self, 10, std::format("CreateQueue(desc:{})", hex_qwords(d, 4)), XBEM_RETURN_ADDRESS());
  auto* q = new QueueState;
  {
    std::lock_guard lock(g_queues_mutex);
    g_queues.push_back(q);
  }
  return out_object(out, make_probe_object("dstorage.queue", &queue_vtable(), q));
}

const ProbeVtable& factory_vtable() {
  static const ProbeVtable v = make_probe_vtable({{4, reinterpret_cast<void*>(&factory_open_file)},
                                                  {5, reinterpret_cast<void*>(&factory_create_status_array)},
                                                  {10, reinterpret_cast<void*>(&factory_create_queue)}});
  return v;
}

// HRESULT DStorageGetFactory(REFIID riid, void** factory)
HRESULT XBEM_GUEST_ABI hle_DStorageGetFactory(const void*, void** out) {
  static void* factory = make_probe_object("dstorage.factory", &factory_vtable());
  return out_object(out, factory);
}

}  // namespace

std::string dstorage_report() {
  std::string out;
  std::lock_guard lock(g_queues_mutex);
  for (size_t i = 0; i < g_queues.size(); ++i) {
    QueueState* q = g_queues[i];
    std::lock_guard ql(q->mutex);
    if (q->pending.empty() && q->ready.empty()) continue;
    out += std::format("{}queue {}: {} waiting for Submit, {} not run yet", out.empty() ? "" : "; ", i,
                       q->pending.size(), q->ready.size());
  }
  for (size_t i = 0; i < g_status_arrays.size(); ++i) {
    StatusState* st = g_status_arrays[i];
    std::lock_guard sl(st->mutex);
    uint32_t open = 0;
    for (uint8_t c : st->complete) open += c ? 0 : 1;
    if (open) out += std::format("{}status array {}: {} of {} incomplete", out.empty() ? "" : "; ", i, open, st->complete.size());
  }
  return out;
}

ModuleDef make_dstorage_module() {
  ModuleDef m;
  m.name = "xcrdapi.dll";
  m.description = "Console DirectStorage: files, queues, status arrays; requests read and inflated by a worker thread per queue";
  m.exports = {XBEM_HLE(DStorageGetFactory)};
  return m;
}

}  // namespace xbem::hle
