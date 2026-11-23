// HLE of the GDK game runtime (XGameRuntime): runtime init, XSystem device
// information, XPackage identity and the XTaskQueue work-dispatch system that
// all GDK async APIs are built on. Services that need Xbox network/user
// infrastructure (XUser, XGameSave, XStore, ...) report E_NOTIMPL for now.
//
// Note: how a title binds to these functions (exported names vs. a function
// table obtained at XGameRuntimeInitialize) must be verified against a real
// title with `xbem inspect` - the names below follow the public GDK headers.

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

#include "hle/discovery.h"
#include "hle/hle_util.h"
#include "platform/memory.h"

namespace xbem::hle {
namespace {

using namespace win;

// -------------------------------------------------------------- XTaskQueue

enum class DispatchMode : uint32_t { Manual = 0, ThreadPool = 1, SerializedThreadPool = 2, Immediate = 3 };
enum : uint32_t { kPortWork = 0, kPortCompletion = 1 };

using TaskCallback = void(XBEM_GUEST_ABI*)(void* context, bool canceled);
using TerminatedCallback = void(XBEM_GUEST_ABI*)(void* context);

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

  TaskQueue(DispatchMode work, DispatchMode completion) {
    ports[kPortWork].mode = work;
    ports[kPortCompletion].mode = completion;
  }

  std::mutex mutex;
  std::condition_variable cv;
  Port ports[2];
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

// XTaskQueueHandle values point at one of these; duplicating a handle creates a new ref.
struct QueueRef {
  uint64_t magic = 0x5154425845ull;  // "EXBTQ"
  std::shared_ptr<TaskQueue> queue;
};

std::mutex g_queue_mutex;
QueueRef* g_process_queue = nullptr;

TaskQueue* queue_from(uint64_t handle) {
  if (!handle) {
    std::lock_guard lock(g_queue_mutex);
    if (!g_process_queue)
      g_process_queue = new QueueRef{0x5154425845ull,
                                     std::make_shared<TaskQueue>(DispatchMode::ThreadPool, DispatchMode::ThreadPool)};
    return g_process_queue->queue.get();
  }
  auto* ref = reinterpret_cast<QueueRef*>(handle);
  return ref->magic == 0x5154425845ull ? ref->queue.get() : nullptr;
}

std::shared_ptr<TaskQueue> shared_queue(uint64_t handle) {
  if (!handle) {
    queue_from(0);
    return g_process_queue->queue;
  }
  return reinterpret_cast<QueueRef*>(handle)->queue;
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
  *out = reinterpret_cast<uint64_t>(new QueueRef{0x5154425845ull, std::move(q)});
  return S_OK;
}

HRESULT XBEM_GUEST_ABI hle_XTaskQueueDuplicateHandle(uint64_t handle, uint64_t* out) {
  if (!out || !queue_from(handle)) return E_INVALIDARG;
  *out = reinterpret_cast<uint64_t>(new QueueRef{0x5154425845ull, shared_queue(handle)});
  return S_OK;
}

void XBEM_GUEST_ABI hle_XTaskQueueCloseHandle(uint64_t handle) {
  if (!handle) return;
  auto* ref = reinterpret_cast<QueueRef*>(handle);
  if (ref->magic != 0x5154425845ull || ref == g_process_queue) return;
  ref->magic = 0;
  delete ref;
}

HRESULT submit(uint64_t handle, uint32_t port, uint32_t delay_ms, void* context, uint64_t callback) {
  if (port > 1 || !callback) return E_INVALIDARG;
  auto q = shared_queue(handle);
  if (!q) return E_INVALIDARG;
  DispatchMode mode;
  {
    std::lock_guard lock(q->mutex);
    if (q->terminated) return static_cast<HRESULT>(0x80070015);  // E_ABORT-like: queue terminated
    mode = q->ports[port].mode;
    if (!(mode == DispatchMode::Immediate && delay_ms == 0)) {
      q->ports[port].items.push_back(
          {callback, reinterpret_cast<uint64_t>(context),
           std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms)});
    }
  }
  if (mode == DispatchMode::Immediate && delay_ms == 0) {
    reinterpret_cast<TaskCallback>(callback)(context, false);
    return S_OK;
  }
  q->cv.notify_all();
  if (mode != DispatchMode::Manual) ensure_worker(q, port);
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
  TaskQueue* q = queue_from(queue);
  if (!q || port > 1) return false;
  TaskQueue::Item item;
  if (!q->pop(port, timeout_ms, item)) return false;
  reinterpret_cast<TaskCallback>(item.callback)(reinterpret_cast<void*>(item.context), false);
  return true;
}

bool XBEM_GUEST_ABI hle_XTaskQueueIsEmpty(uint64_t queue, uint32_t port) {
  TaskQueue* q = queue_from(queue);
  if (!q || port > 1) return true;
  std::lock_guard lock(q->mutex);
  return q->ports[port].items.empty();
}

HRESULT XBEM_GUEST_ABI hle_XTaskQueueTerminate(uint64_t queue, bool, void* context, uint64_t callback) {
  TaskQueue* q = queue_from(queue);
  if (!q) return E_INVALIDARG;
  {
    std::lock_guard lock(q->mutex);
    q->terminated = true;
  }
  q->cv.notify_all();
  if (callback) reinterpret_cast<TerminatedCallback>(callback)(context);
  return S_OK;
}

bool XBEM_GUEST_ABI hle_XTaskQueueGetCurrentProcessTaskQueue(uint64_t* out) {
  if (!out) return false;
  queue_from(0);
  return hle_XTaskQueueDuplicateHandle(reinterpret_cast<uint64_t>(g_process_queue), out) == S_OK;
}

void XBEM_GUEST_ABI hle_XTaskQueueSetCurrentProcessTaskQueue(uint64_t queue) {
  if (!queue_from(queue)) return;
  std::lock_guard lock(g_queue_mutex);
  g_process_queue = new QueueRef{0x5154425845ull, shared_queue(queue)};
}

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
  return copy_id("XDKS.1", size, buffer, used);  // the default development sandbox
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
HRESULT XBEM_GUEST_ABI hle_QueryApiImpl(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
  log_binding_call("QueryApiImpl", a, b, c, d);
  return E_NOINTERFACE;
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

ModuleDef make_xgameruntime_module() {
  ModuleDef m;
  m.name = "xgameruntime.dll";
  m.description = "GDK game runtime: XSystem, XPackage, XTaskQueue (XUser/XGameSave/XStore stubbed)";
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
