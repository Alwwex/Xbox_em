#include <algorithm>
#include "platform/thread.h"

#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <asm/prctl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <climits>
#endif

#include "platform/memory.h"

namespace xbem::platform {

unsigned hardware_threads() {
  const unsigned n = std::thread::hardware_concurrency();
  return n ? n : 1;
}

void yield_thread() { std::this_thread::yield(); }

#if defined(_WIN32)
void flush_process_write_buffers() { FlushProcessWriteBuffers(); }
#else
void flush_process_write_buffers() {
  // membarrier(PRIVATE_EXPEDITED) issues a barrier on every CPU running a
  // thread of this process. Fall back to the classic mprotect trick, whose
  // TLB shootdown has the same effect, if the kernel lacks it.
  constexpr int kRegisterPrivateExpedited = 1 << 4, kPrivateExpedited = 1 << 3;
  static const bool have_membarrier = syscall(SYS_membarrier, kRegisterPrivateExpedited, 0, 0) == 0;
  if (have_membarrier && syscall(SYS_membarrier, kPrivateExpedited, 0, 0) == 0) return;
  static void* page = map_anywhere(page_size(), page_size(), Prot::RW);
  if (!page) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return;
  }
  static_cast<volatile char*>(page)[0] = 1;  // make sure it is populated
  protect(page, page_size(), Prot::R);
  protect(page, page_size(), Prot::RW);
}
#endif

#if defined(_WIN32)

bool guest_execution_supported(std::string* reason) {
  if (reason)
    *reason =
        "native Windows hosts cannot redirect the GS segment to an emulated TEB yet; "
        "run xbem inside WSL2 (Linux) for now";
  return false;
}

namespace {
struct StartBlock {
  ThreadFn fn;
  void* arg;
};
DWORD WINAPI win_thread_main(LPVOID p) {
  StartBlock* sb = static_cast<StartBlock*>(p);
  ThreadFn fn = sb->fn;
  void* arg = sb->arg;
  delete sb;
  fn(arg);
  return 0;
}
}  // namespace

bool start_thread(size_t stack_size, ThreadFn fn, void* arg, NativeThread& out) {
  auto* sb = new StartBlock{fn, arg};
  HANDLE h = CreateThread(nullptr, stack_size, win_thread_main, sb, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
  if (!h) {
    delete sb;
    return false;
  }
  out.handle = reinterpret_cast<uintptr_t>(h);
  return true;
}

void join_thread(NativeThread& t) {
  if (!t.handle) return;
  WaitForSingleObject(reinterpret_cast<HANDLE>(t.handle), INFINITE);
  CloseHandle(reinterpret_cast<HANDLE>(t.handle));
  t.handle = 0;
}

bool set_thread_affinity(const NativeThread& t, uint64_t mask) {
  return t.handle && SetThreadAffinityMask(reinterpret_cast<HANDLE>(t.handle), static_cast<DWORD_PTR>(mask)) != 0;
}

void detach_thread(NativeThread& t) {
  if (!t.handle) return;
  CloseHandle(reinterpret_cast<HANDLE>(t.handle));
  t.handle = 0;
}

bool current_stack_bounds(uintptr_t& low, uintptr_t& high) {
  ULONG_PTR lo = 0, hi = 0;
  GetCurrentThreadStackLimits(&lo, &hi);
  low = lo;
  high = hi;
  return true;
}

bool set_guest_teb(void*) { return false; }
void set_current_thread_name(const std::string&) {}
uint32_t current_processor_number() { return GetCurrentProcessorNumber(); }
uint32_t host_process_id() { return GetCurrentProcessId(); }

#else

bool guest_execution_supported(std::string* reason) {
#if defined(__x86_64__)
  if (reason) reason->clear();
  return true;
#else
  if (reason) *reason = "guest code is x86-64 and is executed natively; this host is not x86-64";
  return false;
#endif
}

namespace {
struct StartBlock {
  ThreadFn fn;
  void* arg;
};
void* posix_thread_main(void* p) {
  StartBlock* sb = static_cast<StartBlock*>(p);
  ThreadFn fn = sb->fn;
  void* arg = sb->arg;
  delete sb;
  fn(arg);
  return nullptr;
}
}  // namespace

bool start_thread(size_t stack_size, ThreadFn fn, void* arg, NativeThread& out) {
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  size_t size = align_up(stack_size < 256 * 1024 ? 256 * 1024 : stack_size, page_size());
  if (size < static_cast<size_t>(PTHREAD_STACK_MIN)) size = PTHREAD_STACK_MIN;
  pthread_attr_setstacksize(&attr, size);
  auto* sb = new StartBlock{fn, arg};
  pthread_t tid;
  const int rc = pthread_create(&tid, &attr, posix_thread_main, sb);
  pthread_attr_destroy(&attr);
  if (rc != 0) {
    delete sb;
    return false;
  }
  out.handle = out.id = static_cast<uintptr_t>(tid);
  return true;
}

void join_thread(NativeThread& t) {
  if (!t.handle) return;
  pthread_join(static_cast<pthread_t>(t.handle), nullptr);
  t.handle = 0;
}

bool set_thread_affinity(const NativeThread& t, uint64_t mask) {
  if (!t.id) return false;
  cpu_set_t set;
  CPU_ZERO(&set);
  const unsigned n = std::min(64u, hardware_threads());
  for (unsigned i = 0; i < n; ++i)
    if (mask & (1ull << i)) CPU_SET(i, &set);
  if (CPU_COUNT(&set) == 0) return false;
  return pthread_setaffinity_np(static_cast<pthread_t>(t.id), sizeof set, &set) == 0;
}

void detach_thread(NativeThread& t) {
  if (!t.handle) return;
  pthread_detach(static_cast<pthread_t>(t.handle));
  t.handle = 0;
}

bool current_stack_bounds(uintptr_t& low, uintptr_t& high) {
  pthread_attr_t attr;
  if (pthread_getattr_np(pthread_self(), &attr) != 0) return false;
  void* addr = nullptr;
  size_t size = 0;
  const bool ok = pthread_attr_getstack(&attr, &addr, &size) == 0;
  pthread_attr_destroy(&attr);
  if (!ok) return false;
  // Keep one page of distance from the guard page.
  low = reinterpret_cast<uintptr_t>(addr) + page_size();
  high = reinterpret_cast<uintptr_t>(addr) + size;
  return true;
}

bool set_guest_teb(void* teb) {
#if defined(__x86_64__)
  return syscall(SYS_arch_prctl, ARCH_SET_GS, reinterpret_cast<unsigned long>(teb)) == 0;
#else
  (void)teb;
  return false;
#endif
}

void set_current_thread_name(const std::string& name) {
  pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
}

uint32_t current_processor_number() {
  const int cpu = sched_getcpu();
  return cpu < 0 ? 0u : static_cast<uint32_t>(cpu);
}

uint32_t host_process_id() { return static_cast<uint32_t>(getpid()); }

#endif

}  // namespace xbem::platform
