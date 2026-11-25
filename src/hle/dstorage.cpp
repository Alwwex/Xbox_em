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
// Requests are executed synchronously when they are enqueued (file read, then
// decompression), so a status entry or fence enqueued after them is complete
// at once. The request structure is the console's: it is decoded by locating
// the file object among its fields (the emulator knows its file objects) -
// the offset and the compressed size follow it - and the destination buffer
// and size before it. The first requests are logged in full.

#include <atomic>
#include <fstream>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include "common/inflate.h"
#include "common/strings.h"
#include "hle/d3d12_device.h"
#include "hle/d3d12_internal.h"
#include "hle/discovery.h"
#include "hle/file_log.h"
#include "hle/hle_util.h"
#include "hle/probe_com.h"
#include "hle/registry.h"
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

  bool read(uint64_t offset, uint8_t* out, size_t n) {
    std::lock_guard lock(mutex);
    if (!stream.is_open()) return false;
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(offset));
    stream.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(n));
    return stream.gcount() == static_cast<std::streamsize>(n);
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
};

std::string hex_qwords(const uint64_t* q, int n) {
  std::string s;
  for (int i = 0; i < n; ++i) s += std::format(" {:016x}", q[i]);
  return s;
}

// Executes one request; returns its HRESULT.
HRESULT execute_request(QueueState* q, const uint8_t* request, uint64_t caller) {
  uint64_t w[12] = {};
  if (!request || !platform::safe_read(reinterpret_cast<uint64_t>(request), w, sizeof w)) return E_POINTER;
  const bool log = g_logged_requests.fetch_add(1) < 6;
  if (log) XBEM_INFO("dstorage", "EnqueueRequest from {}:{}", describe_code_address(caller), hex_qwords(w, 12));

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
  if (!dest || !source_size || source_size > (256u << 20)) return E_INVALIDARG;

  std::vector<uint8_t> buffer(source_size);
  if (!file->read(offset, buffer.data(), source_size)) {
    XBEM_WARN("dstorage", "{}: read of {:#x} bytes at {:#x} failed (file size {:#x})", file->guest_path, source_size,
              offset, file->size);
    return HRESULT(0x80070026);  // HRESULT_FROM_WIN32(ERROR_HANDLE_EOF)
  }
  std::lock_guard lock(q->mutex);
  ++q->requests;
  q->bytes_read += source_size;
  if (compression == 0) {
    std::memcpy(dest, buffer.data(), std::min<uint64_t>(source_size, dest_size));
    q->bytes_out += std::min<uint64_t>(source_size, dest_size);
    return S_OK;
  }
  size_t produced = 0;
  const InflateResult r = inflate_auto(buffer.data(), buffer.size(), dest, dest_size, produced);
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

HRESULT XBEM_GUEST_ABI queue_enqueue_request(ProbeObject* self, const uint8_t* request) {
  auto* q = static_cast<QueueState*>(self->state);
  const HRESULT hr = execute_request(q, request, XBEM_RETURN_ADDRESS());
  if (hr < 0) {
    std::lock_guard lock(q->mutex);
    if (q->first_error >= 0) q->first_error = hr;
    if (hr == E_NOTIMPL) ++q->memory_sources;
    else ++q->failed;
  }
  return S_OK;  // EnqueueRequest itself returns void on the PC; errors surface through status arrays
}

void XBEM_GUEST_ABI queue_enqueue_status(ProbeObject* self, ProbeObject* array, uint32_t index) {
  auto* q = static_cast<QueueState*>(self->state);
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
    s->complete[index] = 1;
    s->result[index] = hr;
  }
}

void XBEM_GUEST_ABI queue_enqueue_signal(ProbeObject*, void* fence, uint64_t value) {
  signal_d3d12_fence(fence, value);
}

// 12 EnqueueSetEvent(HANDLE) in the PC interface: the event is set when the
// requests before it complete - which they already have. A handle that is no
// event (the console slot meaning something else) is only logged.
HRESULT XBEM_GUEST_ABI queue_enqueue_set_event(ProbeObject* self, uint64_t handle) {
  auto event = proc().handles().get_as<rt::EventObject>(handle);
  if (event) event->set();
  probe_log(self, 12,
            std::format("EnqueueSetEvent({:#x}) -> {}", handle, event ? "set (requests complete)" : "not an event"),
            XBEM_RETURN_ADDRESS());
  return S_OK;
}

void XBEM_GUEST_ABI queue_submit(ProbeObject* self) {
  auto* q = static_cast<QueueState*>(self->state);
  static std::atomic<int> logged{0};
  const int n = logged.fetch_add(1);
  if (n < 64 || (n & (n - 1)) == 0) {
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
  if (r) state->stream.open(r->host_path, std::ios::binary);
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
  state->complete.assign(std::min<uint32_t>(capacity, 1 << 20), 0);
  state->result.assign(state->complete.size(), S_OK);
  probe_log(self, 5, std::format("CreateStatusArray({})", capacity), XBEM_RETURN_ADDRESS());
  return out_object(out, make_probe_object("dstorage.status_array", &status_vtable(), state));
}

// 10 (console): queue creation (desc, riid, ppv)
HRESULT XBEM_GUEST_ABI factory_create_queue(ProbeObject* self, const uint8_t* desc, const void*, void** out) {
  uint64_t d[4] = {};
  if (desc) platform::safe_read(reinterpret_cast<uint64_t>(desc), d, sizeof d);
  probe_log(self, 10, std::format("CreateQueue(desc:{})", hex_qwords(d, 4)), XBEM_RETURN_ADDRESS());
  return out_object(out, make_probe_object("dstorage.queue", &queue_vtable(), new QueueState));
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

ModuleDef make_dstorage_module() {
  ModuleDef m;
  m.name = "xcrdapi.dll";
  m.description = "Console DirectStorage: files, queues, status arrays; requests read and inflated synchronously";
  m.exports = {XBEM_HLE(DStorageGetFactory)};
  return m;
}

}  // namespace xbem::hle
