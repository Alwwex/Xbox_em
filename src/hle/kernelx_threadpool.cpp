// kernelx: the Windows thread pool (work items, timers, waits, simple
// callbacks, cleanup groups, QueueUserWorkItem).
//
// Callbacks run on guest worker threads (created through Process so they
// have a TEB and TLS). Timers and waits are tracked by two host helper
// threads that only decide *when* a callback is due and queue it.

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <thread>

#include "hle/hle_util.h"
#include "platform/thread.h"

namespace xbem::hle {
namespace {

using namespace win;
using Clock = std::chrono::steady_clock;

// --------------------------------------------------------- callback instance

struct CallbackInstance {
  HANDLE set_event = 0;
  HANDLE release_semaphore = 0;
  LONG semaphore_count = 0;
  HANDLE release_mutex = 0;
  void* leave_critical_section = nullptr;
};

void finish_instance(const CallbackInstance& inst) {
  if (inst.leave_critical_section) {
    using LeaveCs = void(XBEM_GUEST_ABI*)(void*);
    auto look = Registry::instance().find("kernelx.dll", "LeaveCriticalSection");
    if (look.exp) reinterpret_cast<LeaveCs>(look.exp->address)(inst.leave_critical_section);
  }
  if (inst.release_mutex)
    if (auto m = proc().handles().get_as<rt::MutexObject>(inst.release_mutex)) m->release(current_tid());
  if (inst.release_semaphore)
    if (auto s = proc().handles().get_as<rt::SemaphoreObject>(inst.release_semaphore))
      s->release(inst.semaphore_count, nullptr);
  if (inst.set_event)
    if (auto e = proc().handles().get_as<rt::EventObject>(inst.set_event)) e->set();
}

// ------------------------------------------------------------------- pool

class Pool {
 public:
  using Task = std::function<void(CallbackInstance*)>;

  void submit(Task task) {
    bool spawn = false;
    {
      std::lock_guard lock(mutex_);
      tasks_.push_back(std::move(task));
      if (idle_ == 0 && workers_ < max_workers()) {
        ++workers_;
        spawn = true;
      }
    }
    cv_.notify_one();
    if (spawn && !proc().create_thread(reinterpret_cast<uint64_t>(&Pool::worker), reinterpret_cast<uint64_t>(this), 0,
                                       false)) {
      std::lock_guard lock(mutex_);
      --workers_;
      XBEM_ERROR("threadpool", "cannot start a worker thread");
    }
  }

 private:
  static int max_workers() { return static_cast<int>(std::max(4u, platform::hardware_threads())); }

  static DWORD XBEM_GUEST_ABI worker(void* raw) {
    auto* pool = static_cast<Pool*>(raw);
    platform::set_current_thread_name("tp-worker");
    for (;;) {
      Task task;
      {
        std::unique_lock lock(pool->mutex_);
        ++pool->idle_;
        pool->cv_.wait(lock, [&] { return !pool->tasks_.empty(); });
        --pool->idle_;
        task = std::move(pool->tasks_.front());
        pool->tasks_.pop_front();
      }
      CallbackInstance inst;
      task(&inst);
      finish_instance(inst);
    }
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Task> tasks_;
  int workers_ = 0;
  int idle_ = 0;
};

Pool& pool() {
  static Pool* p = new Pool();  // never destroyed: workers live as long as the process
  return *p;
}

// ------------------------------------------------- shared object machinery
// Each TP_WORK/TP_TIMER/TP_WAIT tracks queued and running callbacks so that
// WaitFor*Callbacks can block until they are done and Close can free it.

struct TpObject {
  enum class Kind { Work, Timer, Wait } kind;
  uint64_t callback;
  uint64_t context;
  std::mutex mutex;
  std::condition_variable cv;
  int queued = 0;
  int running = 0;
  uint64_t epoch = 0;  // bumped to cancel queued callbacks
  std::atomic<int> refs{1};
  // Timer state.
  bool armed = false;
  Clock::time_point due;
  uint32_t period_ms = 0;
  // Wait state.
  std::shared_ptr<rt::KernelObject> wait_object;
  bool wait_has_deadline = false;

  TpObject(Kind k, uint64_t cb, uint64_t ctx) : kind(k), callback(cb), context(ctx) {}
};

void release(TpObject* o) {
  if (--o->refs == 0) delete o;
}

using WorkCallback = void(XBEM_GUEST_ABI*)(CallbackInstance*, void*, TpObject*);
using WaitCallback = void(XBEM_GUEST_ABI*)(CallbackInstance*, void*, TpObject*, DWORD);

void queue_callback(TpObject* o, DWORD wait_result = 0) {
  uint64_t epoch;
  {
    std::lock_guard lock(o->mutex);
    ++o->queued;
    epoch = o->epoch;
  }
  ++o->refs;
  pool().submit([o, epoch, wait_result](CallbackInstance* inst) {
    bool run;
    {
      std::lock_guard lock(o->mutex);
      --o->queued;
      run = epoch == o->epoch;
      if (run) ++o->running;
    }
    if (run) {
      if (o->kind == TpObject::Kind::Wait)
        reinterpret_cast<WaitCallback>(o->callback)(inst, reinterpret_cast<void*>(o->context), o, wait_result);
      else
        reinterpret_cast<WorkCallback>(o->callback)(inst, reinterpret_cast<void*>(o->context), o);
      std::lock_guard lock(o->mutex);
      --o->running;
    }
    o->cv.notify_all();
    release(o);
  });
}

void wait_callbacks(TpObject* o, bool cancel_pending) {
  std::unique_lock lock(o->mutex);
  if (cancel_pending) ++o->epoch;
  o->cv.wait(lock, [&] { return o->running == 0 && (cancel_pending || o->queued == 0); });
}

// Relative (negative, 100 ns) or absolute (FILETIME) due times.
Clock::time_point due_time(const void* filetime) {
  int64_t v = 0;
  if (filetime) std::memcpy(&v, filetime, 8);
  if (v <= 0) return Clock::now() + std::chrono::microseconds(-v / 10);
  using namespace std::chrono;
  const int64_t unix_100ns = v - 116444736000000000LL;
  const auto target = system_clock::time_point(duration_cast<system_clock::duration>(nanoseconds(unix_100ns * 100)));
  return Clock::now() + duration_cast<Clock::duration>(target - system_clock::now());
}

// ------------------------------------------------------------------ timers

class TimerThread {
 public:
  void arm(TpObject* t) {
    {
      std::lock_guard lock(mutex_);
      if (std::find(timers_.begin(), timers_.end(), t) == timers_.end()) {
        ++t->refs;
        timers_.push_back(t);
      }
      start();
    }
    cv_.notify_all();
  }

  void wake() { cv_.notify_all(); }

 private:
  void start() {
    if (started_) return;
    started_ = true;
    std::thread([this] { run(); }).detach();
  }

  void run() {
    std::unique_lock lock(mutex_);
    for (;;) {
      const auto now = Clock::now();
      auto next = now + std::chrono::seconds(1);
      for (auto it = timers_.begin(); it != timers_.end();) {
        TpObject* t = *it;
        bool fire = false, keep = true;
        {
          std::lock_guard tl(t->mutex);
          if (!t->armed) {
            keep = false;
          } else if (t->due <= now) {
            fire = true;
            if (t->period_ms) t->due += std::chrono::milliseconds(t->period_ms);
            else t->armed = false;
          }
          if (t->armed) next = std::min(next, t->due);
        }
        if (fire) queue_callback(t);
        if (!keep) {
          release(t);
          it = timers_.erase(it);
        } else {
          ++it;
        }
      }
      cv_.wait_until(lock, next);
    }
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<TpObject*> timers_;
  bool started_ = false;
};

TimerThread& timers() {
  static TimerThread* t = new TimerThread();
  return *t;
}

// ------------------------------------------------------------------- waits

class WaitThread {
 public:
  void arm(TpObject* w) {
    {
      std::lock_guard lock(rt::sync_mutex());
      if (std::find(waits_.begin(), waits_.end(), w) == waits_.end()) {
        ++w->refs;
        waits_.push_back(w);
      }
      if (!started_) {
        started_ = true;
        std::thread([this] { run(); }).detach();
      }
    }
    rt::sync_cv().notify_all();
  }

 private:
  // Runs under sync_mutex so object state cannot change between check and acquire.
  void run() {
    std::unique_lock lock(rt::sync_mutex());
    for (;;) {
      const auto now = Clock::now();
      auto next = now + std::chrono::milliseconds(200);
      std::vector<std::pair<TpObject*, DWORD>> ready;
      for (auto it = waits_.begin(); it != waits_.end();) {
        TpObject* w = *it;
        std::unique_lock wl(w->mutex);
        if (!w->armed || !w->wait_object) {
          wl.unlock();
          release(w);
          it = waits_.erase(it);
          continue;
        }
        if (w->wait_object->is_signaled(0)) {
          w->wait_object->acquire(0);
          w->armed = false;
          ready.emplace_back(w, WAIT_OBJECT_0);
        } else if (w->wait_has_deadline && w->due <= now) {
          w->armed = false;
          ready.emplace_back(w, WAIT_TIMEOUT);
        } else if (w->wait_has_deadline) {
          next = std::min(next, w->due);
        }
        ++it;
      }
      if (!ready.empty()) {
        lock.unlock();
        for (auto [w, result] : ready) queue_callback(w, result);
        lock.lock();
        continue;
      }
      rt::sync_cv().wait_until(lock, next);
    }
  }

  std::vector<TpObject*> waits_;  // guarded by sync_mutex
  bool started_ = false;
};

WaitThread& waits() {
  static WaitThread* w = new WaitThread();
  return *w;
}

// ---------------------------------------------------------- cleanup groups

struct CleanupGroup {
  std::mutex mutex;
  std::vector<TpObject*> members;
};

// TP_CALLBACK_ENVIRON_V3: Version, Pool, CleanupGroup, ...
void join_cleanup_group(const void* env, TpObject* o) {
  if (!env) return;
  uint64_t group = 0;
  std::memcpy(&group, static_cast<const uint8_t*>(env) + 16, 8);
  if (!group) return;
  auto* g = reinterpret_cast<CleanupGroup*>(group);
  std::lock_guard lock(g->mutex);
  ++o->refs;
  g->members.push_back(o);
}

TpObject* create(TpObject::Kind kind, uint64_t cb, void* ctx, const void* env) {
  if (!cb) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return nullptr;
  }
  auto* o = new TpObject(kind, cb, reinterpret_cast<uint64_t>(ctx));
  join_cleanup_group(env, o);
  return o;
}

// ------------------------------------------------------------------- API

TpObject* XBEM_GUEST_ABI hle_CreateThreadpoolWork(uint64_t cb, void* ctx, const void* env) {
  return create(TpObject::Kind::Work, cb, ctx, env);
}
void XBEM_GUEST_ABI hle_SubmitThreadpoolWork(TpObject* w) {
  if (w) queue_callback(w);
}
void XBEM_GUEST_ABI hle_WaitForThreadpoolWorkCallbacks(TpObject* w, BOOL cancel) {
  if (w) wait_callbacks(w, cancel);
}
void XBEM_GUEST_ABI hle_CloseThreadpoolWork(TpObject* w) {
  if (w) release(w);
}

TpObject* XBEM_GUEST_ABI hle_CreateThreadpoolTimer(uint64_t cb, void* ctx, const void* env) {
  return create(TpObject::Kind::Timer, cb, ctx, env);
}
BOOL XBEM_GUEST_ABI hle_SetThreadpoolTimerEx(TpObject* t, const void* due, DWORD period, DWORD) {
  if (!t) return FALSE;
  bool was;
  {
    std::lock_guard lock(t->mutex);
    was = t->armed;
    t->armed = due != nullptr;
    if (due) {
      t->due = due_time(due);
      t->period_ms = period;
    }
  }
  if (due) timers().arm(t);
  else timers().wake();
  return was ? TRUE : FALSE;
}
void XBEM_GUEST_ABI hle_SetThreadpoolTimer(TpObject* t, const void* due, DWORD period, DWORD window) {
  hle_SetThreadpoolTimerEx(t, due, period, window);
}
BOOL XBEM_GUEST_ABI hle_IsThreadpoolTimerSet(TpObject* t) {
  if (!t) return FALSE;
  std::lock_guard lock(t->mutex);
  return t->armed ? TRUE : FALSE;
}
void XBEM_GUEST_ABI hle_WaitForThreadpoolTimerCallbacks(TpObject* t, BOOL cancel) {
  if (!t) return;
  if (cancel) {
    std::lock_guard lock(t->mutex);
    t->armed = false;
  }
  wait_callbacks(t, cancel);
}
void XBEM_GUEST_ABI hle_CloseThreadpoolTimer(TpObject* t) {
  if (!t) return;
  {
    std::lock_guard lock(t->mutex);
    t->armed = false;
  }
  timers().wake();
  release(t);
}

TpObject* XBEM_GUEST_ABI hle_CreateThreadpoolWait(uint64_t cb, void* ctx, const void* env) {
  return create(TpObject::Kind::Wait, cb, ctx, env);
}
BOOL XBEM_GUEST_ABI hle_SetThreadpoolWaitEx(TpObject* w, HANDLE h, const void* timeout, void*) {
  if (!w) return FALSE;
  auto object = h ? lookup_handle(h) : nullptr;
  bool was;
  {
    std::lock_guard lock(w->mutex);
    was = w->armed;
    w->armed = object && object->waitable();
    w->wait_object = w->armed ? object : nullptr;
    w->wait_has_deadline = timeout != nullptr;
    if (timeout) w->due = due_time(timeout);
  }
  if (h && !object) XBEM_WARN("threadpool", "SetThreadpoolWait on an unknown handle {:#x}", h);
  if (w->armed) waits().arm(w);
  return was ? TRUE : FALSE;
}
void XBEM_GUEST_ABI hle_SetThreadpoolWait(TpObject* w, HANDLE h, const void* timeout) {
  hle_SetThreadpoolWaitEx(w, h, timeout, nullptr);
}
void XBEM_GUEST_ABI hle_WaitForThreadpoolWaitCallbacks(TpObject* w, BOOL cancel) {
  if (!w) return;
  if (cancel) {
    std::lock_guard lock(w->mutex);
    w->armed = false;
  }
  wait_callbacks(w, cancel);
}
void XBEM_GUEST_ABI hle_CloseThreadpoolWait(TpObject* w) {
  if (!w) return;
  {
    std::lock_guard lock(w->mutex);
    w->armed = false;
  }
  release(w);
}

using SimpleCallback = void(XBEM_GUEST_ABI*)(CallbackInstance*, void*);
BOOL XBEM_GUEST_ABI hle_TrySubmitThreadpoolCallback(uint64_t cb, void* ctx, const void*) {
  if (!cb) return FALSE;
  pool().submit([cb, ctx](CallbackInstance* inst) { reinterpret_cast<SimpleCallback>(cb)(inst, ctx); });
  return TRUE;
}

BOOL XBEM_GUEST_ABI hle_QueueUserWorkItem(uint64_t fn, void* ctx, ULONG) {
  if (!fn) return FALSE;
  pool().submit([fn, ctx](CallbackInstance*) { reinterpret_cast<GuestThreadProc>(fn)(ctx); });
  return TRUE;
}

// A single shared pool backs every PTP_POOL.
uint64_t g_pool_token = 0x544F4F50;  // "POOT"
void* XBEM_GUEST_ABI hle_CreateThreadpool(void*) { return &g_pool_token; }
void XBEM_GUEST_ABI hle_CloseThreadpool(void*) {}
void XBEM_GUEST_ABI hle_SetThreadpoolThreadMaximum(void*, DWORD) {}
BOOL XBEM_GUEST_ABI hle_SetThreadpoolThreadMinimum(void*, DWORD) { return TRUE; }

void* XBEM_GUEST_ABI hle_CreateThreadpoolCleanupGroup() { return new CleanupGroup(); }
void XBEM_GUEST_ABI hle_CloseThreadpoolCleanupGroupMembers(CleanupGroup* g, BOOL cancel, void*) {
  if (!g) return;
  std::vector<TpObject*> members;
  {
    std::lock_guard lock(g->mutex);
    members.swap(g->members);
  }
  for (TpObject* o : members) {
    {
      std::lock_guard lock(o->mutex);
      o->armed = false;
    }
    wait_callbacks(o, cancel);
    release(o);  // the group's reference
    release(o);  // the caller's reference: members are closed by this call
  }
}
void XBEM_GUEST_ABI hle_CloseThreadpoolCleanupGroup(CleanupGroup* g) { delete g; }

// Callback-instance helpers.
BOOL XBEM_GUEST_ABI hle_CallbackMayRunLong(CallbackInstance*) { return TRUE; }
void XBEM_GUEST_ABI hle_DisassociateCurrentThreadFromCallback(CallbackInstance*) {}
void XBEM_GUEST_ABI hle_SetEventWhenCallbackReturns(CallbackInstance* i, HANDLE e) {
  if (i) i->set_event = e;
}
void XBEM_GUEST_ABI hle_ReleaseSemaphoreWhenCallbackReturns(CallbackInstance* i, HANDLE s, DWORD n) {
  if (!i) return;
  i->release_semaphore = s;
  i->semaphore_count = static_cast<LONG>(n);
}
void XBEM_GUEST_ABI hle_ReleaseMutexWhenCallbackReturns(CallbackInstance* i, HANDLE m) {
  if (i) i->release_mutex = m;
}
void XBEM_GUEST_ABI hle_LeaveCriticalSectionWhenCallbackReturns(CallbackInstance* i, void* cs) {
  if (i) i->leave_critical_section = cs;
}
void XBEM_GUEST_ABI hle_FreeLibraryWhenCallbackReturns(CallbackInstance*, HMODULE) {}

}  // namespace

void add_kernelx_threadpool_exports(std::vector<ExportDef>& out) {
  out.insert(out.end(), {
      XBEM_HLE(CreateThreadpoolWork), XBEM_HLE(SubmitThreadpoolWork), XBEM_HLE(WaitForThreadpoolWorkCallbacks),
      XBEM_HLE(CloseThreadpoolWork), XBEM_HLE(CreateThreadpoolTimer), XBEM_HLE(SetThreadpoolTimer),
      XBEM_HLE(SetThreadpoolTimerEx), XBEM_HLE(IsThreadpoolTimerSet), XBEM_HLE(WaitForThreadpoolTimerCallbacks),
      XBEM_HLE(CloseThreadpoolTimer), XBEM_HLE(CreateThreadpoolWait), XBEM_HLE(SetThreadpoolWait),
      XBEM_HLE(SetThreadpoolWaitEx), XBEM_HLE(WaitForThreadpoolWaitCallbacks), XBEM_HLE(CloseThreadpoolWait),
      XBEM_HLE(TrySubmitThreadpoolCallback), XBEM_HLE(QueueUserWorkItem), XBEM_HLE(CreateThreadpool),
      XBEM_HLE(CloseThreadpool), XBEM_HLE(SetThreadpoolThreadMaximum), XBEM_HLE(SetThreadpoolThreadMinimum),
      XBEM_HLE(CreateThreadpoolCleanupGroup), XBEM_HLE(CloseThreadpoolCleanupGroupMembers),
      XBEM_HLE(CloseThreadpoolCleanupGroup), XBEM_HLE(CallbackMayRunLong),
      XBEM_HLE(DisassociateCurrentThreadFromCallback), XBEM_HLE(SetEventWhenCallbackReturns),
      XBEM_HLE(ReleaseSemaphoreWhenCallbackReturns), XBEM_HLE(ReleaseMutexWhenCallbackReturns),
      XBEM_HLE(LeaveCriticalSectionWhenCallbackReturns), XBEM_HLE(FreeLibraryWhenCallbackReturns),
  });
}

}  // namespace xbem::hle
