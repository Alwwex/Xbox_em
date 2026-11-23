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
//            5 EnqueueSignal(fence, value), 6 Submit, 8 Close
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
#include <mutex>
#include <set>
#include <vector>

#include "common/inflate.h"
#include "common/strings.h"
#include "hle/d3d12_device.h"
#include "hle/discovery.h"
#include "hle/hle_util.h"
#include "hle/probe_com.h"
#include "hle/registry.h"
#include "platform/memory.h"

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
  const uint32_t uncompressed_size = static_cast<uint32_t>(w[fi + 2] >> 32);
  const uint32_t compression = static_cast<uint32_t>(w[0] & 0xFF);
  // Destination memory: the first pointer before the file, its size the next
  // non-zero word.
  uint8_t* dest = nullptr;
  uint64_t dest_size = 0;
  for (int i = 1; i < fi; ++i) {
    if (!dest && w[i] >= 0x10000 && w[i] < 0x800000000000ull) {
      dest = reinterpret_cast<uint8_t*>(w[i]);
    } else if (dest && !dest_size && w[i] && w[i] < (1ull << 32)) {
      dest_size = w[i];
    }
  }
  if (!dest_size) dest_size = uncompressed_size ? uncompressed_size : source_size;
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
                "first bytes {:02x} {:02x} {:02x} {:02x}",
                file->guest_path, compression, offset, source_size, inflate_result_name(r), produced, buffer[0],
                buffer.size() > 1 ? buffer[1] : 0, buffer.size() > 2 ? buffer[2] : 0, buffer.size() > 3 ? buffer[3] : 0);
    return HRESULT(0x80070017);  // HRESULT_FROM_WIN32(ERROR_CRC): corrupt data
  }
  return S_OK;
}

HRESULT XBEM_GUEST_ABI queue_enqueue_request(ProbeObject* self, const uint8_t* request) {
  auto* q = static_cast<QueueState*>(self->state);
  const HRESULT hr = execute_request(q, request, XBEM_RETURN_ADDRESS());
  if (hr < 0) {
    std::lock_guard lock(q->mutex);
    if (q->first_error >= 0) q->first_error = hr;
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

void XBEM_GUEST_ABI queue_submit(ProbeObject* self) {
  auto* q = static_cast<QueueState*>(self->state);
  static std::atomic<int> logged{0};
  const int n = logged.fetch_add(1);
  if (n < 3 || (n & (n - 1)) == 0) {
    std::lock_guard lock(q->mutex);
    XBEM_INFO("dstorage", "Submit #{}: {} requests so far, {:#x} bytes read, {:#x} bytes delivered", n + 1,
              q->requests, q->bytes_read, q->bytes_out);
  }
}

const ProbeVtable& queue_vtable() {
  static const ProbeVtable v = make_probe_vtable({{3, reinterpret_cast<void*>(&queue_enqueue_request)},
                                                  {4, reinterpret_cast<void*>(&queue_enqueue_status)},
                                                  {5, reinterpret_cast<void*>(&queue_enqueue_signal)},
                                                  {6, reinterpret_cast<void*>(&queue_submit)}});
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
