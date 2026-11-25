// HLE of the GDK game runtime (XGameRuntime): runtime init, XSystem device
// information, XPackage identity, the XTaskQueue work-dispatch system and the
// XAsync operations all GDK async APIs are built on, and the binding of API
// families through QueryApiImpl (XUser lives in xuser.cpp). Services that
// need Xbox network infrastructure (XGameSave, XStore, ...) are not bound yet.
//
// Note: how a title binds to these functions (exported names vs. a function
// table obtained at XGameRuntimeInitialize) must be verified against a real
// title with `xbem inspect` - the names below follow the public GDK headers.

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include <functional>

#include "hle/discovery.h"
#include "hle/hle_util.h"
#include "hle/probe_com.h"
#include "hle/xasync.h"
#include "runtime/objects.h"
#include "platform/memory.h"

namespace xbem::hle {
namespace {

using namespace win;

// -------------------------------------------------------------- XTaskQueue

enum class DispatchMode : uint32_t { Manual = 0, ThreadPool = 1, SerializedThreadPool = 2, Immediate = 3 };
enum : uint32_t { kPortWork = 0, kPortCompletion = 1 };

using TaskCallback = void(XBEM_GUEST_ABI*)(void* context, bool canceled);
using TerminatedCallback = void(XBEM_GUEST_ABI*)(void* context);
using MonitorCallback = void(XBEM_GUEST_ABI*)(void* context, uint64_t queue, uint32_t port);

class TaskQueue {
 public:
  struct Item {
    uint64_t callback;
    uint64_t context;
    std::chrono::steady_clock::time_point due;
  };
  struct Port {
    DispatchMode mode = DispatchMode::Manual;
    std::deque<Item> items;
    bool worker_started = false;
  };
  struct Monitor {
    uint64_t token, context, callback, handle;
  };

  TaskQueue(DispatchMode work, DispatchMode completion) {
    ports[kPortWork].mode = work;
    ports[kPortCompletion].mode = completion;
  }

  std::mutex mutex;
  std::condition_variable cv;
  Port ports[2];
  std::vector<Monitor> monitors;
  bool terminated = false;

  // Pops an item that is due, waiting up to `timeout_ms`. Caller holds no lock.
  bool pop(uint32_t port, uint32_t timeout_ms, Item& out) {
    std::unique_lock lock(mutex);
    const auto deadline = timeout_ms == INFINITE ? std::chrono::steady_clock::time_point::max()
                                                 : std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
      auto& q = ports[port].items;
      const auto now = std::chrono::steady_clock::now();
      auto next_due = std::chrono::steady_clock::time_point::max();
      for (auto it = q.begin(); it != q.end(); ++it) {
        if (it->due <= now) {
          out = *it;
          q.erase(it);
          return true;
        }
        next_due = std::min(next_due, it->due);
      }
      if (terminated || now >= deadline) return false;
      cv.wait_until(lock, std::min(deadline, next_due));
    }
  }
};

constexpr uint64_t kQueueMagic = 0x5154425845ull;      // "EXBTQ"
constexpr uint64_t kPortMagic = 0x5054425845ull;       // "EXBTP"

// One port of a queue: where the items submitted to a queue handle's port go.
struct PortTarget {
  std::shared_ptr<TaskQueue> queue;
  uint32_t port = 0;
};

// XTaskQueueHandle values point at one of these; duplicating a handle creates
// a new ref. A composite queue (XTaskQueueCreateComposite) routes its two
// ports to ports of other queues.
struct QueueRef {
  uint64_t magic = kQueueMagic;
  PortTarget ports[2];
};

// XTaskQueuePortHandle values.
struct PortRef {
  uint64_t magic = kPortMagic;
  PortTarget target;
};

std::mutex g_queue_mutex;
QueueRef* g_process_queue = nullptr;

QueueRef* new_queue_ref(const std::shared_ptr<TaskQueue>& q) {
  return new QueueRef{kQueueMagic, {{q, kPortWork}, {q, kPortCompletion}}};
}

// The queue handle `handle` names; 0 is the process task queue.
QueueRef* queue_ref(uint64_t handle) {
  if (!handle) {
    std::lock_guard lock(g_queue_mutex);
    if (!g_process_queue)
      g_process_queue = new_queue_ref(std::make_shared<TaskQueue>(DispatchMode::ThreadPool, DispatchMode::ThreadPool));
    return g_process_queue;
  }
  auto* ref = reinterpret_cast<QueueRef*>(handle);
  uint64_t magic = 0;
  platform::safe_read(handle, &magic, sizeof magic);
  return magic == kQueueMagic ? ref : nullptr;
}

TaskQueue* queue_from(uint64_t handle) {
  QueueRef* ref = queue_ref(handle);
  return ref ? ref->ports[kPortWork].queue.get() : nullptr;
}

struct WorkerArgs {
  std::shared_ptr<TaskQueue> queue;
  uint32_t port;
};

// Runs as a guest thread (created through Process::create_thread) so that
// callbacks execute with a proper TEB/TLS.
DWORD XBEM_GUEST_ABI queue_worker(void* raw) {
  std::unique_ptr<WorkerArgs> args(static_cast<WorkerArgs*>(raw));
  TaskQueue::Item item;
  while (args->queue->pop(args->port, INFINITE, item))
    reinterpret_cast<TaskCallback>(item.callback)(reinterpret_cast<void*>(item.context), false);
  return 0;
}

void ensure_worker(const std::shared_ptr<TaskQueue>& q, uint32_t port) {
  {
    std::lock_guard lock(q->mutex);
    if (q->ports[port].worker_started) return;
    q->ports[port].worker_started = true;
  }
  auto* args = new WorkerArgs{q, port};
  if (!proc().create_thread(reinterpret_cast<uint64_t>(&queue_worker), reinterpret_cast<uint64_t>(args), 0, false)) {
    XBEM_ERROR("xgameruntime", "cannot start XTaskQueue worker thread");
    delete args;
  }
}

HRESULT XBEM_GUEST_ABI hle_XTaskQueueCreate(uint32_t work_mode, uint32_t completion_mode, uint64_t* out) {
  if (!out || work_mode > 3 || completion_mode > 3) return E_INVALIDARG;
  auto q = std::make_shared<TaskQueue>(static_cast<DispatchMode>(work_mode), static_cast<DispatchMode>(completion_mode));
  *out = reinterpret_cast<uint64_t>(new_queue_ref(q));
  return S_OK;
}

HRESULT XBEM_GUEST_ABI hle_XTaskQueueGetPort(uint64_t handle, uint32_t port, uint64_t* out) {
  QueueRef* ref = queue_ref(handle);
  if (!ref || port > 1 || !out) return E_INVALIDARG;
  static std::mutex lock;
  static std::map<std::pair<TaskQueue*, uint32_t>, PortRef*> handles;  // port handles live as long as the process
  std::lock_guard guard(lock);
  PortRef*& p = handles[{ref->ports[port].queue.get(), ref->ports[port].port}];
  if (!p) p = new PortRef{kPortMagic, ref->ports[port]};
  *out = reinterpret_cast<uint64_t>(p);
  return S_OK;
}

HRESULT XBEM_GUEST_ABI hle_XTaskQueueCreateComposite(uint64_t work_port, uint64_t completion_port, uint64_t* out) {
  auto* w = reinterpret_cast<PortRef*>(work_port);
  auto* c = reinterpret_cast<PortRef*>(completion_port);
  if (!w || !c || !out || w->magic != kPortMagic || c->magic != kPortMagic) return E_INVALIDARG;
  *out = reinterpret_cast<uint64_t>(new QueueRef{kQueueMagic, {w->target, c->target}});
  return S_OK;
}

HRESULT XBEM_GUEST_ABI hle_XTaskQueueDuplicateHandle(uint64_t handle, uint64_t* out) {
  QueueRef* ref = queue_ref(handle);
  if (!out || !ref) return E_INVALIDARG;
  *out = reinterpret_cast<uint64_t>(new QueueRef{kQueueMagic, {ref->ports[0], ref->ports[1]}});
  return S_OK;
}

void XBEM_GUEST_ABI hle_XTaskQueueCloseHandle(uint64_t handle) {
  QueueRef* ref = handle ? queue_ref(handle) : nullptr;
  if (!ref || ref == g_process_queue) return;
  ref->magic = 0;
  delete ref;
}

HRESULT submit(uint64_t handle, uint32_t port, uint32_t delay_ms, void* context, uint64_t callback) {
  if (port > 1 || !callback) return E_INVALIDARG;
  QueueRef* ref = queue_ref(handle);
  if (!ref) return E_INVALIDARG;
  const PortTarget target = ref->ports[port];
  auto& q = target.queue;
  DispatchMode mode;
  std::vector<TaskQueue::Monitor> monitors;
  {
    std::lock_guard lock(q->mutex);
    if (q->terminated) return static_cast<HRESULT>(0x80070015);  // E_ABORT-like: queue terminated
    mode = q->ports[target.port].mode;
    if (!(mode == DispatchMode::Immediate && delay_ms == 0)) {
      q->ports[target.port].items.push_back(
          {callback, reinterpret_cast<uint64_t>(context),
           std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms)});
    }
    monitors = q->monitors;
  }
  if (mode == DispatchMode::Immediate && delay_ms == 0) {
    reinterpret_cast<TaskCallback>(callback)(context, false);
    return S_OK;
  }
  q->cv.notify_all();
  if (mode != DispatchMode::Manual) ensure_worker(q, target.port);
  for (const auto& m : monitors)
    reinterpret_cast<MonitorCallback>(m.callback)(reinterpret_cast<void*>(m.context), m.handle, target.port);
  return S_OK;
}

HRESULT XBEM_GUEST_ABI hle_XTaskQueueSubmitCallback(uint64_t queue, uint32_t port, void* context, uint64_t callback) {
  return submit(queue, port, 0, context, callback);
}

HRESULT XBEM_GUEST_ABI hle_XTaskQueueSubmitDelayedCallback(uint64_t queue, uint32_t port, uint32_t delay_ms,
                                                           void* context, uint64_t callback) {
  return submit(queue, port, delay_ms, context, callback);
}

bool XBEM_GUEST_ABI hle_XTaskQueueDispatch(uint64_t queue, uint32_t port, uint32_t timeout_ms) {
  QueueRef* ref = queue_ref(queue);
  if (!ref || port > 1) return false;
  TaskQueue::Item item;
  if (!ref->ports[port].queue->pop(ref->ports[port].port, timeout_ms, item)) return false;
  reinterpret_cast<TaskCallback>(item.callback)(reinterpret_cast<void*>(item.context), false);
  return true;
}

bool XBEM_GUEST_ABI hle_XTaskQueueIsEmpty(uint64_t queue, uint32_t port) {
  QueueRef* ref = queue_ref(queue);
  if (!ref || port > 1) return true;
  auto& q = ref->ports[port].queue;
  std::lock_guard lock(q->mutex);
  return q->ports[ref->ports[port].port].items.empty();
}

HRESULT XBEM_GUEST_ABI hle_XTaskQueueTerminate(uint64_t queue, bool, void* context, uint64_t callback) {
  QueueRef* ref = queue_ref(queue);
  if (!ref) return E_INVALIDARG;
  for (auto& target : ref->ports) {
    {
      std::lock_guard lock(target.queue->mutex);
      target.queue->terminated = true;
    }
    target.queue->cv.notify_all();
  }
  if (callback) reinterpret_cast<TerminatedCallback>(callback)(context);
  return S_OK;
}

std::atomic<uint64_t> g_next_token{1};

// XTaskQueueRegisterWaiter: a host thread waits for the kernel object and
// submits the callback when it is signaled (once, as the GDK does).
struct Waiter {
  std::atomic<bool> cancelled{false};
};
std::mutex g_waiter_mutex;
std::map<uint64_t, std::shared_ptr<Waiter>> g_waiters;

HRESULT XBEM_GUEST_ABI hle_XTaskQueueRegisterWaiter(uint64_t queue, uint32_t port, uint64_t wait_handle, void* context,
                                                    uint64_t callback, uint64_t* token) {
  if (!queue_ref(queue) || port > 1 || !callback || !token) return E_INVALIDARG;
  auto object = proc().handles().get(wait_handle);
  if (!object) return E_INVALIDARG;
  auto waiter = std::make_shared<Waiter>();
  const uint64_t t = g_next_token++;
  {
    std::lock_guard lock(g_waiter_mutex);
    g_waiters[t] = waiter;
  }
  std::thread([=] {
    const std::shared_ptr<rt::KernelObject> objects[] = {object};
    while (!waiter->cancelled) {
      if (rt::wait_for_objects(objects, false, 100, 0) == 0) {
        if (!waiter->cancelled) submit(queue, port, 0, context, callback);
        break;
      }
    }
  }).detach();
  *token = t;
  return S_OK;
}

void XBEM_GUEST_ABI hle_XTaskQueueUnregisterWaiter(uint64_t, uint64_t token) {
  std::lock_guard lock(g_waiter_mutex);
  auto it = g_waiters.find(token);
  if (it == g_waiters.end()) return;
  it->second->cancelled = true;
  g_waiters.erase(it);
}

HRESULT XBEM_GUEST_ABI hle_XTaskQueueRegisterMonitor(uint64_t queue, void* context, uint64_t callback, uint64_t* token) {
  QueueRef* ref = queue_ref(queue);
  if (!ref || !callback || !token) return E_INVALIDARG;
  const uint64_t t = g_next_token++;
  for (auto* q : {ref->ports[0].queue.get(), ref->ports[1].queue.get()}) {
    std::lock_guard lock(q->mutex);
    q->monitors.push_back({t, reinterpret_cast<uint64_t>(context), callback, queue});
    if (ref->ports[0].queue == ref->ports[1].queue) break;
  }
  *token = t;
  return S_OK;
}

void XBEM_GUEST_ABI hle_XTaskQueueUnregisterMonitor(uint64_t queue, uint64_t token) {
  QueueRef* ref = queue_ref(queue);
  if (!ref) return;
  for (auto& target : ref->ports) {
    std::lock_guard lock(target.queue->mutex);
    std::erase_if(target.queue->monitors, [&](const TaskQueue::Monitor& m) { return m.token == token; });
  }
}

bool XBEM_GUEST_ABI hle_XTaskQueueGetCurrentProcessTaskQueue(uint64_t* out) {
  if (!out) return false;
  return hle_XTaskQueueDuplicateHandle(reinterpret_cast<uint64_t>(queue_ref(0)), out) == S_OK;
}

void XBEM_GUEST_ABI hle_XTaskQueueSetCurrentProcessTaskQueue(uint64_t queue) {
  QueueRef* ref = queue_ref(queue);
  if (!ref) return;
  auto* copy = new QueueRef{kQueueMagic, {ref->ports[0], ref->ports[1]}};
  std::lock_guard lock(g_queue_mutex);
  g_process_queue = copy;
}

// ------------------------------------------------------------------ XAsync
//
// XAsyncBlock { queue, context, callback, internal[32] }: the runtime keeps
// each block's state keyed by its address. A guest provider is called with
// XAsyncOp Begin (from XAsyncBegin), DoWork (on the work port after
// XAsyncSchedule), GetResult, Cancel and Cleanup; the completion callback
// runs on the completion port.

struct XAsyncBlock {
  uint64_t queue;
  uint64_t context;
  uint64_t callback;
  uint8_t internal[32];
};
static_assert(sizeof(XAsyncBlock) == 56);

struct XAsyncProviderData {
  uint64_t async;
  size_t buffer_size;
  void* buffer;
  uint64_t context;
};

enum XAsyncOp : uint32_t { kOpBegin = 0, kOpDoWork = 1, kOpGetResult = 2, kOpCancel = 3, kOpCleanup = 4 };

using XAsyncProvider = HRESULT(XBEM_GUEST_ABI*)(uint32_t op, const XAsyncProviderData* data);
using XAsyncWork = HRESULT(XBEM_GUEST_ABI*)(uint64_t async);
using XAsyncCompletion = void(XBEM_GUEST_ABI*)(uint64_t async);

constexpr HRESULT E_ABORT = static_cast<HRESULT>(0x80004004);
constexpr HRESULT E_NOT_SUFFICIENT_BUFFER = static_cast<HRESULT>(0x8007007A);

struct AsyncState {
  uint64_t block = 0;
  uint64_t identity = 0;
  uint64_t provider = 0;  // guest XAsyncProvider, or 0
  uint64_t work = 0;      // guest XAsyncWork (XAsyncRun), or 0
  uint64_t context = 0;
  std::function<void(void*)> host_write;  // host operations: writes the result
  std::mutex mutex;
  std::condition_variable cv;
  HRESULT status = kEPending;
  size_t required = 0;
  bool cleaned = false;
};

std::mutex g_async_mutex;
std::map<uint64_t, std::shared_ptr<AsyncState>> g_async;

std::shared_ptr<AsyncState> async_state(uint64_t block) {
  std::lock_guard lock(g_async_mutex);
  auto it = g_async.find(block);
  return it == g_async.end() ? nullptr : it->second;
}

std::shared_ptr<AsyncState> async_begin_state(uint64_t block, uint64_t identity) {
  auto state = std::make_shared<AsyncState>();
  state->block = block;
  state->identity = identity;
  std::lock_guard lock(g_async_mutex);
  g_async[block] = state;  // a block is reused once its previous operation completed
  return state;
}

HRESULT call_provider(AsyncState& s, uint32_t op, size_t size = 0, void* buffer = nullptr) {
  if (!s.provider) return S_OK;
  XAsyncProviderData data{s.block, size, buffer, s.context};
  return reinterpret_cast<XAsyncProvider>(s.provider)(op, &data);
}

void cleanup(AsyncState& s) {
  {
    std::lock_guard lock(s.mutex);
    if (s.cleaned) return;
    s.cleaned = true;
  }
  call_provider(s, kOpCleanup);
}

// XAsyncComplete: the first completion wins.
void complete(const std::shared_ptr<AsyncState>& s, HRESULT result, size_t required) {
  {
    std::lock_guard lock(s->mutex);
    if (s->status != kEPending) return;
    s->status = result;
    s->required = result < 0 ? 0 : required;
  }
  s->cv.notify_all();
  if (result < 0 || required == 0) cleanup(*s);
  XAsyncBlock block{};
  platform::safe_read(s->block, &block, sizeof block);
  if (block.callback) submit(block.queue, kPortCompletion, 0, reinterpret_cast<void*>(s->block), block.callback);
}

// The work-port callback XAsyncSchedule submits (context = the block).
void XBEM_GUEST_ABI async_do_work(void* context, bool canceled) {
  auto s = async_state(reinterpret_cast<uint64_t>(context));
  if (!s) return;
  HRESULT hr;
  if (canceled)
    hr = E_ABORT;
  else if (s->work)
    hr = reinterpret_cast<XAsyncWork>(s->work)(s->block);
  else
    hr = call_provider(*s, kOpDoWork);
  if (hr != kEPending) complete(s, hr, 0);
}

HRESULT XBEM_GUEST_ABI hle_XAsyncBegin(uint64_t async, uint64_t context, uint64_t identity, const char*,
                                       uint64_t provider) {
  if (!async || !provider) return E_INVALIDARG;
  auto s = async_begin_state(async, identity);
  s->provider = provider;
  s->context = context;
  const HRESULT hr = call_provider(*s, kOpBegin);
  if (hr < 0) {
    {
      std::lock_guard lock(s->mutex);
      s->status = hr;
    }
    cleanup(*s);
  }
  return hr;
}

HRESULT XBEM_GUEST_ABI hle_XAsyncSchedule(uint64_t async, uint32_t delay_ms) {
  auto s = async_state(async);
  if (!s) return E_INVALIDARG;
  XAsyncBlock block{};
  platform::safe_read(async, &block, sizeof block);
  return submit(block.queue, kPortWork, delay_ms, reinterpret_cast<void*>(async),
                reinterpret_cast<uint64_t>(&async_do_work));
}

void XBEM_GUEST_ABI hle_XAsyncComplete(uint64_t async, HRESULT result, size_t required) {
  if (auto s = async_state(async)) complete(s, result, required);
}

HRESULT XBEM_GUEST_ABI hle_XAsyncRun(uint64_t async, uint64_t work) {
  if (!async || !work) return E_INVALIDARG;
  auto s = async_begin_state(async, 0);
  s->work = work;
  return hle_XAsyncSchedule(async, 0);
}

HRESULT XBEM_GUEST_ABI hle_XAsyncGetStatus(uint64_t async, bool wait) {
  auto s = async_state(async);
  if (!s) return E_INVALIDARG;
  std::unique_lock lock(s->mutex);
  if (wait) s->cv.wait(lock, [&] { return s->status != kEPending; });
  return s->status;
}

HRESULT XBEM_GUEST_ABI hle_XAsyncGetResultSize(uint64_t async, size_t* size) {
  auto s = async_state(async);
  if (!s || !size) return E_INVALIDARG;
  std::lock_guard lock(s->mutex);
  if (s->status < 0 || s->status == kEPending) return s->status;
  *size = s->required;
  return S_OK;
}

HRESULT XBEM_GUEST_ABI hle_XAsyncCancel(uint64_t async) {
  auto s = async_state(async);
  if (!s) return E_INVALIDARG;
  {
    std::lock_guard lock(s->mutex);
    if (s->status != kEPending) return S_OK;
  }
  call_provider(*s, kOpCancel);
  complete(s, E_ABORT, 0);
  return S_OK;
}

HRESULT XBEM_GUEST_ABI hle_XAsyncGetResult(uint64_t async, uint64_t identity, size_t size, void* buffer,
                                           size_t* used) {
  auto s = async_state(async);
  if (!s) return E_INVALIDARG;
  size_t required;
  {
    std::lock_guard lock(s->mutex);
    if (s->status < 0 || s->status == kEPending) return s->status;
    if (identity && s->identity && identity != s->identity) return E_INVALIDARG;
    required = s->required;
  }
  if (required == 0) {
    if (used) *used = 0;
    return S_OK;
  }
  if (!buffer || size < required) return E_NOT_SUFFICIENT_BUFFER;
  HRESULT hr = S_OK;
  if (s->host_write)
    s->host_write(buffer);
  else
    hr = call_provider(*s, kOpGetResult, size, buffer);
  if (used) *used = required;
  cleanup(*s);
  return hr;
}

}  // namespace

HRESULT xasync_run_host(uint64_t block, const void* identity, HRESULT status, size_t size,
                        std::function<void(void*)> write) {
  if (!block) return E_INVALIDARG;
  auto s = async_begin_state(block, reinterpret_cast<uint64_t>(identity));
  s->host_write = std::move(write);
  complete(s, status, size);
  return S_OK;
}

HRESULT xasync_result(uint64_t block, const void* identity, size_t size, void* buffer, size_t* used) {
  return hle_XAsyncGetResult(block, reinterpret_cast<uint64_t>(identity), size, buffer, used);
}

HRESULT xasync_status(uint64_t block, bool wait) { return hle_XAsyncGetStatus(block, wait); }

namespace {

// ------------------------------------------------------------------ XThread

bool XBEM_GUEST_ABI hle_XThreadIsTimeSensitive() { return false; }
HRESULT XBEM_GUEST_ABI hle_XThreadSetTimeSensitive(bool) { return S_OK; }
void XBEM_GUEST_ABI hle_XThreadAssertNotTimeSensitive() {}


// --------------------------------------------------------------- runtime

HRESULT XBEM_GUEST_ABI hle_XGameRuntimeInitialize() {
  XBEM_INFO("xgameruntime", "XGameRuntimeInitialize");
  queue_from(0);  // the process task queue exists from here on
  return S_OK;
}

HRESULT XBEM_GUEST_ABI hle_XGameRuntimeInitializeWithOptions(const void*) { return hle_XGameRuntimeInitialize(); }
void XBEM_GUEST_ABI hle_XGameRuntimeUninitialize() { XBEM_INFO("xgameruntime", "XGameRuntimeUninitialize"); }
bool XBEM_GUEST_ABI hle_XGameRuntimeIsFeatureAvailable(uint32_t feature) {
  XBEM_DEBUG("xgameruntime", "XGameRuntimeIsFeatureAvailable({}) -> false", feature);
  return false;
}

// ---------------------------------------------------------------- XSystem

uint32_t XBEM_GUEST_ABI hle_XSystemGetDeviceType() { return static_cast<uint32_t>(proc().options().device); }

HRESULT copy_id(std::string_view value, size_t size, char* buffer, size_t* used) {
  if (!buffer || size < value.size() + 1) return E_INVALIDARG;
  std::memcpy(buffer, value.data(), value.size());
  buffer[value.size()] = 0;
  if (used) *used = value.size() + 1;
  return S_OK;
}

HRESULT XBEM_GUEST_ABI hle_XSystemGetConsoleId(size_t size, char* buffer, size_t* used) {
  return copy_id("0000.0000.0000.0000.0000.0000.0000.000", size, buffer, used);  // 38 chars + NUL = 39 bytes
}

HRESULT XBEM_GUEST_ABI hle_XSystemGetXboxLiveSandboxId(size_t size, char* buffer, size_t* used) {
  // A retail console's sandbox. GTA VI compares it with RETAIL, CERT, CERT.DEBUG
  // and RKTR.* (fn 0x3dda560) and asserts on any other (the development
  // sandbox XDKS.1 included).
  return copy_id("RETAIL", size, buffer, used);
}

HRESULT XBEM_GUEST_ABI hle_XSystemGetAppSpecificDeviceId(size_t size, char* buffer, size_t* used) {
  return copy_id("XBEM-EMULATED-DEVICE", size, buffer, used);
}

struct XSystemAnalyticsInfo {
  uint16_t os_major, os_minor, os_build, os_revision;
  uint16_t host_major, host_minor, host_build, host_revision;
  char family[64];
  char form[64];
};
static_assert(sizeof(XSystemAnalyticsInfo) == 144);

XSystemAnalyticsInfo XBEM_GUEST_ABI hle_XSystemGetAnalyticsInfo() {
  XSystemAnalyticsInfo info{};
  info.os_major = info.host_major = 10;
  info.os_build = info.host_build = 25398;
  std::snprintf(info.family, sizeof info.family, "Xbox");
  const bool lockhart = proc().options().device == rt::DeviceType::XboxScarlettLockhart;
  std::snprintf(info.form, sizeof info.form, "%s", lockhart ? "Xbox Series S" : "Xbox Series X");
  return info;
}

// ---------------------------------------------------------------- XPackage

bool XBEM_GUEST_ABI hle_XPackageIsPackagedProcess() { return true; }

HRESULT XBEM_GUEST_ABI hle_XPackageGetCurrentProcessPackageIdentifier(size_t size, char* buffer) {
  const auto& cfg = proc().layout().config;
  const std::string id = cfg && !cfg->identity_name.empty() ? cfg->identity_name : "xbem.UnknownTitle";
  return copy_id(id, size, buffer, nullptr);
}

HRESULT XBEM_GUEST_ABI hle_XPackageGetUserLocale(size_t size, char* buffer) { return copy_id("en-US", size, buffer, nullptr); }

// ----------------------------------------------------- not yet supported

#define XBEM_NOTIMPL_HR(fn)                                                                  \
  HRESULT XBEM_GUEST_ABI hle_##fn() {                                                        \
    XBEM_WARN_ONCE("xgameruntime", #fn " is not supported yet -> E_NOTIMPL");                \
    return E_NOTIMPL;                                                                        \
  }

XBEM_NOTIMPL_HR(XUserAddAsync)
XBEM_NOTIMPL_HR(XUserAddByIdWithUiAsync)
XBEM_NOTIMPL_HR(XGameSaveInitializeProvider)
XBEM_NOTIMPL_HR(XGameSaveInitializeProviderAsync)
XBEM_NOTIMPL_HR(XStoreCreateContext)
XBEM_NOTIMPL_HR(XNetworkingGetConnectivityHint)
XBEM_NOTIMPL_HR(XGameUiShowMessageDialogAsync)
XBEM_NOTIMPL_HR(XGameInviteRegisterForEvent)
XBEM_NOTIMPL_HR(XPackageGetMountPath)

// ------------------------------------------------- runtime binding (discovery)
//
// Titles do not import GDK functions by name. The static xgameruntime.lib
// linked into the game loads xgameruntime.dll, calls InitializeApiImpl* and
// then QueryApiImpl to obtain the implementation of each API family. That
// interface is undocumented, so for now these entry points only log what the
// title passes (pointer arguments are dumped as GUIDs and strings) to map it.

void log_binding_call(const char* name, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
  XBEM_WARN("xgameruntime", "{}({}, {}, {}, {})", name, describe_arg(a), describe_arg(b), describe_arg(c),
            describe_arg(d));
}

constexpr HRESULT E_NOINTERFACE = static_cast<HRESULT>(0x80004002);

HRESULT XBEM_GUEST_ABI hle_InitializeApiImpl(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
  log_binding_call("InitializeApiImpl", a, b, c, d);
  return S_OK;
}
HRESULT XBEM_GUEST_ABI hle_InitializeApiImplEx(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
  log_binding_call("InitializeApiImplEx", a, b, c, d);
  return S_OK;
}
HRESULT XBEM_GUEST_ABI hle_InitializeApiImplEx2(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
  log_binding_call("InitializeApiImplEx2", a, b, c, d);
  return S_OK;
}
// API families the runtime library binds through QueryApiImpl(clsid, iid,
// &object). Each object is IUnknown (slots 0-2) followed by the family's
// functions; slots not listed here are probe slots (logged, S_OK).

// XSystem {e349bd1a-fc20-4e40-b99c-4178cc6b409f} / {1861cf2e-e18b-4834-a9f5-b4a4e6efb4cf}:
// slot 3 GetConsoleId, 4 GetXboxLiveSandboxId, 5 GetAppSpecificDeviceId
// (measured: the title's wrappers pass 39 and 45 byte buffers to 3 and 5).
HRESULT XBEM_GUEST_ABI xsystem_console_id(ProbeObject*, size_t size, char* buffer, size_t* used) {
  return hle_XSystemGetConsoleId(size, buffer, used);
}
HRESULT XBEM_GUEST_ABI xsystem_sandbox_id(ProbeObject*, size_t size, char* buffer, size_t* used) {
  return hle_XSystemGetXboxLiveSandboxId(size, buffer, used);
}
HRESULT XBEM_GUEST_ABI xsystem_app_device_id(ProbeObject*, size_t size, char* buffer, size_t* used) {
  return hle_XSystemGetAppSpecificDeviceId(size, buffer, used);
}

constexpr uint8_t kXSystemClsid[16] = {0x1a, 0xbd, 0x49, 0xe3, 0x20, 0xfc, 0x40, 0x4e,
                                       0xb9, 0x9c, 0x41, 0x78, 0xcc, 0x6b, 0x40, 0x9f};

// XNetworking {37e56907-2f10-41e8-b72f-36edb185331a} / {bf2346b2-39af-4658-b5ea-44713c7e83b3}:
// slot 15 GetConnectivityHint(XNetworkingConnectivityHint*) - the title polls
// it from many places and reads connectivityLevel (+0) and networkInitialized
// (+0xc). The emulated console is initialized and offline.
struct ConnectivityHint {
  uint32_t level;  // XNetworkingConnectivityLevelHint: 1 = None
  uint32_t cost;   // XNetworkingConnectivityCostHint: 0 = Unknown
  uint32_t iana_interface_type;
  bool network_initialized, approaching_data_limit, over_data_limit, roaming;
};
static_assert(sizeof(ConnectivityHint) == 16);

HRESULT XBEM_GUEST_ABI xnetworking_connectivity_hint(ProbeObject*, ConnectivityHint* hint) {
  if (!hint) return E_INVALIDARG;
  *hint = {1, 0, 0, true, false, false, false};
  return S_OK;
}

constexpr uint8_t kXNetworkingClsid[16] = {0x07, 0x69, 0xe5, 0x37, 0x10, 0x2f, 0xe8, 0x41,
                                           0xb7, 0x2f, 0x36, 0xed, 0xb1, 0x85, 0x33, 0x1a};

// The XAsync / XTaskQueue / XThread family {073b7dcb-1fcf-4030-94be-e3c9eb623428}
// (clsid = iid). Its slots follow the public headers: XAsync.h (3-6),
// XAsyncProvider.h (7-11), XTaskQueue.h (12-26, with DuplicateHandle moved
// before Dispatch and Terminate after the waiters), XThread.h (27-29).
// Measured: slot 12 is XTaskQueueCreate(ThreadPool, ThreadPool, &queue);
// 16 returns a bool from (queue, port, timeout); 17 takes one handle; 22 is
// (queue, bool wait, context, callback); 29 takes nothing and runs before
// most GDK calls of every family (XThreadAssertNotTimeSensitive).
template <auto F>
struct Slot;
template <class R, class... A, R(XBEM_GUEST_ABI* F)(A...)>
struct Slot<F> {
  static R XBEM_GUEST_ABI call(ProbeObject*, A... args) { return F(args...); }
};
template <auto F>
void* slot() {
  return reinterpret_cast<void*>(&Slot<F>::call);
}

constexpr uint8_t kXAsyncClsid[16] = {0xcb, 0x7d, 0x3b, 0x07, 0xcf, 0x1f, 0x30, 0x40,
                                      0x94, 0xbe, 0xe3, 0xc9, 0xeb, 0x62, 0x34, 0x28};

const ProbeVtable& xasync_vtable() {
  static const ProbeVtable v = gdk_vtable({
      {3, slot<&hle_XAsyncGetStatus>()},
      {4, slot<&hle_XAsyncGetResultSize>()},
      {5, slot<&hle_XAsyncCancel>()},
      {6, slot<&hle_XAsyncRun>()},
      {7, slot<&hle_XAsyncBegin>()},
      {9, slot<&hle_XAsyncSchedule>()},
      {10, slot<&hle_XAsyncComplete>()},
      {11, slot<&hle_XAsyncGetResult>()},
      {12, slot<&hle_XTaskQueueCreate>()},
      {13, slot<&hle_XTaskQueueGetPort>()},
      {14, slot<&hle_XTaskQueueCreateComposite>()},
      {15, slot<&hle_XTaskQueueDuplicateHandle>()},
      {16, slot<&hle_XTaskQueueDispatch>()},
      {17, slot<&hle_XTaskQueueCloseHandle>()},
      {18, slot<&hle_XTaskQueueSubmitCallback>()},
      {19, slot<&hle_XTaskQueueSubmitDelayedCallback>()},
      {20, slot<&hle_XTaskQueueRegisterWaiter>()},
      {21, slot<&hle_XTaskQueueUnregisterWaiter>()},
      {22, slot<&hle_XTaskQueueTerminate>()},
      {23, slot<&hle_XTaskQueueRegisterMonitor>()},
      {24, slot<&hle_XTaskQueueUnregisterMonitor>()},
      {25, slot<&hle_XTaskQueueGetCurrentProcessTaskQueue>()},
      {26, slot<&hle_XTaskQueueSetCurrentProcessTaskQueue>()},
      {27, slot<&hle_XThreadIsTimeSensitive>()},
      {28, slot<&hle_XThreadSetTimeSensitive>()},
      {29, slot<&hle_XThreadAssertNotTimeSensitive>()},
  });
  return v;
}

// XPackage {af406016-e850-4aa8-a88d-2f3dcb9dac7e} / {0b9d556e-...}: 3
// GetCurrentProcessPackageIdentifier(size, buffer) (the title passes 33
// bytes), 4 IsPackagedProcess(), 5 CreateInstallationMonitor(id, count,
// selectors, interval, queue, &monitor), 6 CloseInstallationMonitorHandle,
// 7 GetInstallationProgress(monitor, &progress), 8 UpdateInstallationMonitor,
// 9/10 (Un)RegisterInstallationProgressChanged, 39 EnumeratePackages(kind,
// scope, context, callback) - measured shapes. The game is fully installed
// and no other packages (DLC) are.
HRESULT XBEM_GUEST_ABI xpackage_identifier(ProbeObject*, size_t size, char* buffer) {
  return hle_XPackageGetCurrentProcessPackageIdentifier(size, buffer);
}
bool XBEM_GUEST_ABI xpackage_is_packaged(ProbeObject*) { return true; }
HRESULT XBEM_GUEST_ABI xpackage_enumerate(ProbeObject*, uint32_t kind, uint32_t scope, void*, uint64_t callback) {
  XBEM_DEBUG("xgameruntime", "XPackageEnumeratePackages(kind {}, scope {}, callback {:#x}) -> none", kind, scope,
             callback);
  return S_OK;
}

struct InstallationProgress {
  uint64_t total_bytes, installed_bytes, launch_bytes;
  bool launchable, completed;
};
uint8_t g_installation_monitor;  // the one monitor handle (its address)

HRESULT XBEM_GUEST_ABI xpackage_create_monitor(ProbeObject*, const char*, uint32_t, const void*, uint32_t, uint64_t,
                                               void** monitor) {
  if (!monitor) return E_INVALIDARG;
  *monitor = &g_installation_monitor;
  return S_OK;
}
void XBEM_GUEST_ABI xpackage_nothing(ProbeObject*, uint64_t) {}
void XBEM_GUEST_ABI xpackage_progress(ProbeObject*, void*, InstallationProgress* progress) {
  if (progress) *progress = {1ull << 30, 1ull << 30, 1ull << 30, true, true};
}
HRESULT XBEM_GUEST_ABI xpackage_register_progress(ProbeObject*, void*, void*, uint64_t, uint64_t* token) {
  if (token) *token = 1;
  return S_OK;
}
void XBEM_GUEST_ABI xpackage_unregister_progress(ProbeObject*, void*, uint64_t, bool) {}

constexpr uint8_t kXPackageClsid[16] = {0x16, 0x60, 0x40, 0xaf, 0x50, 0xe8, 0xa8, 0x4a,
                                        0xa8, 0x8d, 0x2f, 0x3d, 0xcb, 0x9d, 0xac, 0x7e};

constexpr uint8_t kXUserClsid[16] = {0x77, 0xd1, 0xac, 0x01, 0xf9, 0x91, 0x63, 0x47,
                                     0xa3, 0x8e, 0xcc, 0xbb, 0x55, 0xce, 0x32, 0xe0};

// The object implementing (`clsid`, `iid`), or null.
void* api_implementation(const uint8_t clsid[16], const uint8_t iid[16]) {
  if (std::memcmp(clsid, kXSystemClsid, 16) == 0) {
    static const ProbeVtable vtable = gdk_vtable({{3, reinterpret_cast<void*>(&xsystem_console_id)},
                                                  {4, reinterpret_cast<void*>(&xsystem_sandbox_id)},
                                                  {5, reinterpret_cast<void*>(&xsystem_app_device_id)}});
    static void* object = make_probe_object("xgameruntime.XSystem", &vtable);
    return object;
  }
  if (std::memcmp(clsid, kXNetworkingClsid, 16) == 0) {
    static const ProbeVtable vtable = gdk_vtable({{15, reinterpret_cast<void*>(&xnetworking_connectivity_hint)}});
    static void* object = make_probe_object("xgameruntime.XNetworking", &vtable);
    return object;
  }
  if (std::memcmp(clsid, kXAsyncClsid, 16) == 0 && std::memcmp(iid, kXAsyncClsid, 16) == 0) {
    static void* object = make_probe_object("xgameruntime.XAsync", &xasync_vtable());
    return object;
  }
  if (std::memcmp(clsid, kXUserClsid, 16) == 0) return xuser_api(iid);
  if (std::memcmp(clsid, kXPackageClsid, 16) == 0) {
    static const ProbeVtable vtable = gdk_vtable({{3, reinterpret_cast<void*>(&xpackage_identifier)},
                                                  {4, reinterpret_cast<void*>(&xpackage_is_packaged)},
                                                  {5, reinterpret_cast<void*>(&xpackage_create_monitor)},
                                                  {6, reinterpret_cast<void*>(&xpackage_nothing)},
                                                  {7, reinterpret_cast<void*>(&xpackage_progress)},
                                                  {8, reinterpret_cast<void*>(&xpackage_nothing)},
                                                  {9, reinterpret_cast<void*>(&xpackage_register_progress)},
                                                  {10, reinterpret_cast<void*>(&xpackage_unregister_progress)},
                                                  {39, reinterpret_cast<void*>(&xpackage_enumerate)}});
    static void* object = make_probe_object("xgameruntime.XPackage", &vtable);
    return object;
  }
  return nullptr;
}

// The title's runtime library queries the implementation again on every call
// of a GDK function it could not bind, so polled functions query it thousands
// of times: the first calls per (class, interface) pair are logged with the
// call site, later ones counted.
HRESULT XBEM_GUEST_ABI hle_QueryApiImpl(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
  static std::mutex lock;
  static std::map<std::pair<std::string, std::string>, uint64_t> calls;
  uint8_t clsid[16] = {}, iid[16] = {};
  platform::safe_read(a, clsid, 16);
  platform::safe_read(b, iid, 16);
  uint64_t n;
  {
    std::lock_guard guard(lock);
    n = ++calls[{guid_to_string(clsid), guid_to_string(iid)}];
  }
  void* object = api_implementation(clsid, iid);
  if (n <= 3 || std::has_single_bit(n))
    XBEM_WARN("xgameruntime", "QueryApiImpl({}, {}, {}) from {} -> {} (call {} for this pair)", describe_arg(a),
              describe_arg(b), describe_arg(c), describe_code_address(XBEM_RETURN_ADDRESS()),
              object ? "S_OK" : "E_NOINTERFACE", n);
  if (!object) return E_NOINTERFACE;
  if (!c) return E_POINTER;
  *reinterpret_cast<void**>(c) = object;
  return S_OK;
}
HRESULT XBEM_GUEST_ABI hle_UninitializeApiImpl(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
  log_binding_call("UninitializeApiImpl", a, b, c, d);
  return S_OK;
}
HRESULT XBEM_GUEST_ABI hle_XErrorReport(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
  log_binding_call("XErrorReport", a, b, c, d);
  return S_OK;
}

}  // namespace

// Slots of a GDK family the emulator does not know yet: E_NOTIMPL, as the
// functions failed before the family was bound (not a probe's S_OK, which
// would leave outputs unwritten while reporting success).
static HRESULT XBEM_GUEST_ABI gdk_not_implemented(ProbeObject* self) {
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 32)
    XBEM_WARN("xgameruntime", "{}: unknown slot called from {} -> E_NOTIMPL", self->name,
              describe_code_address(XBEM_RETURN_ADDRESS()));
  return E_NOTIMPL;
}

ProbeVtable gdk_vtable(std::initializer_list<std::pair<int, void*>> slots) {
  ProbeVtable v = default_probe_vtable();
  for (int i = 3; i < 96; ++i) v[i] = reinterpret_cast<void*>(&gdk_not_implemented);
  for (const auto& [slot, f] : slots) v[slot] = f;
  return v;
}

ModuleDef make_xgameruntime_module() {
  ModuleDef m;
  m.name = "xgameruntime.dll";
  m.description = "GDK game runtime: XSystem, XPackage, XTaskQueue, XAsync, XUser (XGameSave/XStore not bound)";
  m.exports = {
      XBEM_HLE(XGameRuntimeInitialize), XBEM_HLE(XGameRuntimeInitializeWithOptions),
      XBEM_HLE(XGameRuntimeUninitialize), XBEM_HLE(XGameRuntimeIsFeatureAvailable), XBEM_HLE(XTaskQueueCreate),
      XBEM_HLE(XTaskQueueDuplicateHandle), XBEM_HLE(XTaskQueueCloseHandle), XBEM_HLE(XTaskQueueSubmitCallback),
      XBEM_HLE(XTaskQueueSubmitDelayedCallback), XBEM_HLE(XTaskQueueDispatch), XBEM_HLE(XTaskQueueIsEmpty),
      XBEM_HLE(XTaskQueueTerminate), XBEM_HLE(XTaskQueueGetCurrentProcessTaskQueue),
      XBEM_HLE(XTaskQueueSetCurrentProcessTaskQueue), XBEM_HLE(XSystemGetDeviceType), XBEM_HLE(XSystemGetConsoleId),
      XBEM_HLE(XSystemGetXboxLiveSandboxId), XBEM_HLE(XSystemGetAppSpecificDeviceId),
      XBEM_HLE(XSystemGetAnalyticsInfo), XBEM_HLE(XPackageIsPackagedProcess),
      XBEM_HLE(XPackageGetCurrentProcessPackageIdentifier), XBEM_HLE(XPackageGetUserLocale),
      XBEM_HLE_STUB(XUserAddAsync), XBEM_HLE_STUB(XUserAddByIdWithUiAsync), XBEM_HLE_STUB(XGameSaveInitializeProvider),
      XBEM_HLE_STUB(XGameSaveInitializeProviderAsync), XBEM_HLE_STUB(XStoreCreateContext),
      XBEM_HLE_STUB(XNetworkingGetConnectivityHint), XBEM_HLE_STUB(XGameUiShowMessageDialogAsync),
      XBEM_HLE_STUB(XGameInviteRegisterForEvent), XBEM_HLE_STUB(XPackageGetMountPath),
      // Runtime binding used by xgameruntime.lib (discovery mode, see above).
      XBEM_HLE_STUB(InitializeApiImpl), XBEM_HLE_STUB(InitializeApiImplEx), XBEM_HLE_STUB(InitializeApiImplEx2),
      XBEM_HLE_STUB(QueryApiImpl), XBEM_HLE_STUB(UninitializeApiImpl), XBEM_HLE_STUB(XErrorReport),
  };
  return m;
}

}  // namespace xbem::hle
