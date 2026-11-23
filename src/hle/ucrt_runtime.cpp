// UCRT: process startup/shutdown, errno, invalid-parameter handling, argv,
// CRT threads and the heap (malloc & co.).

#include <algorithm>
#include <atomic>
#include <set>

#include "hle/heap.h"
#include "hle/ucrt.h"

namespace xbem::hle {

namespace crt {

namespace {
thread_local int t_errno = 0;
thread_local uint32_t t_doserrno = 0;
}  // namespace

int* errno_ptr() { return &t_errno; }

void invalid_parameter(const char* function) {
  static std::mutex mu;
  static std::set<std::string> seen;
  std::lock_guard lock(mu);
  if (seen.insert(function).second) XBEM_WARN("ucrt", "invalid parameter passed to {}", function);
}

std::string to_narrow(std::u16string_view w) { return utf16_to_utf8(w); }
std::u16string to_wide(std::string_view s) { return utf8_to_utf16(s); }

}  // namespace crt

namespace {

using namespace win;
using crt::set_errno;

using Pvfv = void(XBEM_GUEST_ABI*)();
using Pifv = int32_t(XBEM_GUEST_ABI*)();

// ------------------------------------------------------- errno & friends

int* XBEM_GUEST_ABI hle__errno() { return crt::errno_ptr(); }
uint32_t* XBEM_GUEST_ABI hle___doserrno() { return &crt::t_doserrno; }
int32_t XBEM_GUEST_ABI hle__get_errno(int32_t* v) {
  if (!v) return crt::kEINVAL;
  *v = *crt::errno_ptr();
  return 0;
}
int32_t XBEM_GUEST_ABI hle__set_errno(int32_t v) {
  set_errno(v);
  return 0;
}

uint64_t g_invalid_parameter_handler = 0;

void XBEM_GUEST_ABI hle__invalid_parameter_noinfo() { crt::invalid_parameter("a CRT function (_invalid_parameter_noinfo)"); }

[[noreturn]] void XBEM_GUEST_ABI hle__invalid_parameter_noinfo_noreturn() {
  XBEM_ERROR("ucrt", "_invalid_parameter_noinfo_noreturn: the C runtime detected an invalid parameter; terminating");
  proc().exit_process(0xC0000409);  // STATUS_STACK_BUFFER_OVERRUN, what __fastfail reports
}

void XBEM_GUEST_ABI hle__invalid_parameter(const char16_t* expr, const char16_t* func, const char16_t* file, UINT line,
                                           uint64_t) {
  XBEM_WARN("ucrt", "invalid parameter: {} in {} ({}:{})", hle::narrow(expr), hle::narrow(func), hle::narrow(file),
            line);
}

uint64_t XBEM_GUEST_ABI hle__set_invalid_parameter_handler(uint64_t h) { return std::exchange(g_invalid_parameter_handler, h); }
uint64_t XBEM_GUEST_ABI hle__get_invalid_parameter_handler() { return g_invalid_parameter_handler; }
uint64_t XBEM_GUEST_ABI hle__set_thread_local_invalid_parameter_handler(uint64_t h) { return hle__set_invalid_parameter_handler(h); }
uint64_t XBEM_GUEST_ABI hle__get_thread_local_invalid_parameter_handler() { return g_invalid_parameter_handler; }

// ------------------------------------------------------------ onexit tables

struct OnexitTable {
  Pvfv* first;
  Pvfv* last;
  Pvfv* end;
};

int32_t XBEM_GUEST_ABI hle__initialize_onexit_table(OnexitTable* t) {
  if (!t) return -1;
  t->first = t->last = t->end = nullptr;
  return 0;
}

int32_t XBEM_GUEST_ABI hle__register_onexit_function(OnexitTable* t, Pvfv fn) {
  if (!t) return -1;
  static std::mutex mu;
  std::lock_guard lock(mu);
  if (t->last == t->end) {
    const size_t count = static_cast<size_t>(t->last - t->first);
    const size_t cap = std::max<size_t>(32, count * 2);
    auto* p = static_cast<Pvfv*>(heap_realloc(t->first, cap * sizeof(Pvfv), false));
    if (!p) return -1;
    t->first = p;
    t->last = p + count;
    t->end = p + cap;
  }
  *t->last++ = fn;
  return 0;
}

int32_t XBEM_GUEST_ABI hle__execute_onexit_table(OnexitTable* t) {
  if (!t) return -1;
  Pvfv* first = t->first;
  Pvfv* last = t->last;
  t->first = t->last = t->end = nullptr;
  while (last > first) {
    --last;
    if (*last) (*last)();
  }
  heap_free(first);
  return 0;
}

std::mutex g_atexit_mutex;
std::vector<Pvfv> g_atexit;
std::atomic<bool> g_atexit_ran{false};

void run_atexit_handlers() {
  if (g_atexit_ran.exchange(true)) return;
  for (;;) {
    Pvfv fn;
    {
      std::lock_guard lock(g_atexit_mutex);
      if (g_atexit.empty()) break;
      fn = g_atexit.back();
      g_atexit.pop_back();
    }
    if (fn) fn();
  }
}

int32_t XBEM_GUEST_ABI hle_atexit(Pvfv fn) {
  std::lock_guard lock(g_atexit_mutex);
  g_atexit.push_back(fn);
  return 0;
}
int32_t XBEM_GUEST_ABI hle__crt_atexit(Pvfv fn) { return hle_atexit(fn); }
int32_t XBEM_GUEST_ABI hle__crt_at_quick_exit(Pvfv) { return 0; }
int32_t XBEM_GUEST_ABI hle_at_quick_exit(Pvfv) { return 0; }
Pvfv XBEM_GUEST_ABI hle__onexit(Pvfv fn) { return hle_atexit(fn) == 0 ? fn : nullptr; }

void flush_all() {
  std::fflush(nullptr);
}

[[noreturn]] void XBEM_GUEST_ABI hle_exit(int32_t code) {
  XBEM_INFO("ucrt", "exit({})", code);
  run_atexit_handlers();
  flush_all();
  proc().exit_process(static_cast<uint32_t>(code));
}
[[noreturn]] void XBEM_GUEST_ABI hle__exit(int32_t code) {
  flush_all();
  proc().exit_process(static_cast<uint32_t>(code));
}
[[noreturn]] void XBEM_GUEST_ABI hle__Exit(int32_t code) { hle__exit(code); }
[[noreturn]] void XBEM_GUEST_ABI hle_quick_exit(int32_t code) { hle__exit(code); }
void XBEM_GUEST_ABI hle__cexit() {
  run_atexit_handlers();
  flush_all();
}
void XBEM_GUEST_ABI hle__c_exit() { flush_all(); }

[[noreturn]] void XBEM_GUEST_ABI hle_abort() {
  XBEM_ERROR("ucrt", "abort() called by the guest");
  flush_all();
  proc().exit_process(3);
}
[[noreturn]] void XBEM_GUEST_ABI hle_terminate() {
  XBEM_ERROR("ucrt", "terminate() called by the guest (unhandled C++ exception or noexcept violation)");
  flush_all();
  proc().exit_process(3);
}
UINT XBEM_GUEST_ABI hle__set_abort_behavior(UINT, UINT) { return 0; }

void XBEM_GUEST_ABI hle__initterm(Pvfv* first, Pvfv* last) {
  for (; first < last; ++first)
    if (*first) (*first)();
}

int32_t XBEM_GUEST_ABI hle__initterm_e(Pifv* first, Pifv* last) {
  for (; first < last; ++first) {
    if (!*first) continue;
    if (const int32_t r = (*first)()) return r;
  }
  return 0;
}

int32_t XBEM_GUEST_ABI hle__seh_filter_dll(uint32_t, void*) { return 0; }  // EXCEPTION_CONTINUE_SEARCH
int32_t XBEM_GUEST_ABI hle__seh_filter_exe(uint32_t, void*) { return 0; }
int32_t XBEM_GUEST_ABI hle__register_thread_local_exe_atexit_callback(uint64_t) { return 0; }

// ------------------------------------------------------------------- argv

struct ArgvStorage {
  int32_t argc = 0;
  char** argv = nullptr;
  char16_t** wargv = nullptr;
  char** environ_a = nullptr;
  char16_t** environ_w = nullptr;
  char* acmdln = nullptr;
  char16_t* wcmdln = nullptr;
  char* pgmptr = nullptr;
  char16_t* wpgmptr = nullptr;
  std::vector<std::string> narrow;
  std::vector<std::u16string> wide;
  std::vector<char*> narrow_ptrs;
  std::vector<char16_t*> wide_ptrs;
  char* empty_env_a[1] = {nullptr};
  char16_t* empty_env_w[1] = {nullptr};
};

// CommandLineToArgvW rules: whitespace separates, quotes group, 2n backslashes
// before a quote become n backslashes, 2n+1 escape the quote.
std::vector<std::string> split_command_line(const std::string& cmd) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < cmd.size()) {
    while (i < cmd.size() && (cmd[i] == ' ' || cmd[i] == '\t')) ++i;
    if (i >= cmd.size()) break;
    std::string arg;
    bool quoted = false;
    while (i < cmd.size() && (quoted || (cmd[i] != ' ' && cmd[i] != '\t'))) {
      size_t slashes = 0;
      while (i < cmd.size() && cmd[i] == '\\') {
        ++slashes;
        ++i;
      }
      if (i < cmd.size() && cmd[i] == '"') {
        arg.append(slashes / 2, '\\');
        if (slashes % 2) arg += '"';
        else quoted = !quoted;
        ++i;
      } else {
        arg.append(slashes, '\\');
        if (i < cmd.size() && (quoted || (cmd[i] != ' ' && cmd[i] != '\t'))) arg += cmd[i++];
      }
    }
    out.push_back(std::move(arg));
  }
  return out;
}

ArgvStorage& argv_storage() {
  static ArgvStorage* s = [] {
    auto* a = new ArgvStorage();
    a->narrow = split_command_line(proc().command_line_a());
    for (const auto& n : a->narrow) a->wide.push_back(crt::to_wide(n));
    for (auto& n : a->narrow) a->narrow_ptrs.push_back(n.data());
    a->narrow_ptrs.push_back(nullptr);
    for (auto& w : a->wide) a->wide_ptrs.push_back(w.data());
    a->wide_ptrs.push_back(nullptr);
    a->argc = static_cast<int32_t>(a->narrow.size());
    a->argv = a->narrow_ptrs.data();
    a->wargv = a->wide_ptrs.data();
    a->environ_a = a->empty_env_a;
    a->environ_w = a->empty_env_w;
    a->acmdln = const_cast<char*>(proc().command_line_a().c_str());
    a->wcmdln = const_cast<char16_t*>(proc().command_line_w().c_str());
    a->pgmptr = a->argc ? a->argv[0] : a->acmdln;
    a->wpgmptr = a->argc ? a->wargv[0] : a->wcmdln;
    return a;
  }();
  return *s;
}

int32_t XBEM_GUEST_ABI hle__configure_narrow_argv(int32_t) { return 0; }
int32_t XBEM_GUEST_ABI hle__configure_wide_argv(int32_t) { return 0; }
int32_t XBEM_GUEST_ABI hle__initialize_narrow_environment() { return 0; }
int32_t XBEM_GUEST_ABI hle__initialize_wide_environment() { return 0; }
int32_t* XBEM_GUEST_ABI hle___p___argc() { return &argv_storage().argc; }
char*** XBEM_GUEST_ABI hle___p___argv() { return &argv_storage().argv; }
char16_t*** XBEM_GUEST_ABI hle___p___wargv() { return &argv_storage().wargv; }
char** XBEM_GUEST_ABI hle___p__acmdln() { return &argv_storage().acmdln; }
char16_t** XBEM_GUEST_ABI hle___p__wcmdln() { return &argv_storage().wcmdln; }
char*** XBEM_GUEST_ABI hle___p__environ() { return &argv_storage().environ_a; }
char16_t*** XBEM_GUEST_ABI hle___p__wenviron() { return &argv_storage().environ_w; }
char** XBEM_GUEST_ABI hle__get_initial_narrow_environment() { return argv_storage().environ_a; }
char16_t** XBEM_GUEST_ABI hle__get_initial_wide_environment() { return argv_storage().environ_w; }
char** XBEM_GUEST_ABI hle___p__pgmptr() { return &argv_storage().pgmptr; }
char16_t** XBEM_GUEST_ABI hle___p__wpgmptr() { return &argv_storage().wpgmptr; }

int32_t XBEM_GUEST_ABI hle__get_pgmptr(char** out) {
  if (!out) return crt::kEINVAL;
  *out = argv_storage().pgmptr;
  return 0;
}
int32_t XBEM_GUEST_ABI hle__get_wpgmptr(char16_t** out) {
  if (!out) return crt::kEINVAL;
  *out = argv_storage().wpgmptr;
  return 0;
}

template <class Char>
Char* skip_program_name(Char* p) {
  bool quoted = false;
  while (*p && (quoted || (*p != ' ' && *p != '\t'))) {
    if (*p == '"') quoted = !quoted;
    ++p;
  }
  while (*p == ' ' || *p == '\t') ++p;
  return p;
}
char* XBEM_GUEST_ABI hle__get_narrow_winmain_command_line() { return skip_program_name(argv_storage().acmdln); }
char16_t* XBEM_GUEST_ABI hle__get_wide_winmain_command_line() { return skip_program_name(argv_storage().wcmdln); }

// msvcrt-style startup used by MinGW/older CRTs.
int32_t XBEM_GUEST_ABI hle___getmainargs(int32_t* argc, char*** argv, char*** env, int32_t, void*) {
  ArgvStorage& a = argv_storage();
  if (argc) *argc = a.argc;
  if (argv) *argv = a.argv;
  if (env) *env = a.environ_a;
  return 0;
}
int32_t XBEM_GUEST_ABI hle___wgetmainargs(int32_t* argc, char16_t*** argv, char16_t*** env, int32_t, void*) {
  ArgvStorage& a = argv_storage();
  if (argc) *argc = a.argc;
  if (argv) *argv = a.wargv;
  if (env) *env = a.environ_w;
  return 0;
}

void XBEM_GUEST_ABI hle__set_app_type(int32_t) {}
void XBEM_GUEST_ABI hle___set_app_type(int32_t) {}
void XBEM_GUEST_ABI hle___setusermatherr(uint64_t) {}

int32_t g_fmode = 0x4000;  // _O_TEXT
int32_t g_commode = 0;
int32_t XBEM_GUEST_ABI hle__set_fmode(int32_t m) {
  g_fmode = m;
  return 0;
}
int32_t XBEM_GUEST_ABI hle__get_fmode(int32_t* m) {
  if (!m) return crt::kEINVAL;
  *m = g_fmode;
  return 0;
}
int32_t* XBEM_GUEST_ABI hle___p__fmode() { return &g_fmode; }
int32_t* XBEM_GUEST_ABI hle___p__commode() { return &g_commode; }

// ---------------------------------------------------------------- signals

uint64_t g_signal_handlers[32] = {};

uint64_t XBEM_GUEST_ABI hle_signal(int32_t sig, uint64_t handler) {
  if (sig < 0 || sig >= 32) {
    set_errno(crt::kEINVAL);
    return ~0ull;  // SIG_ERR
  }
  return std::exchange(g_signal_handlers[sig], handler);
}

int32_t XBEM_GUEST_ABI hle_raise(int32_t sig) {
  constexpr int32_t kSigAbrt = 22;  // SIGABRT on Windows
  const uint64_t h = (sig >= 0 && sig < 32) ? g_signal_handlers[sig] : 0;
  if (h > 1) {
    reinterpret_cast<void(XBEM_GUEST_ABI*)(int32_t)>(h)(sig);
    return 0;
  }
  if (h == 1) return 0;  // SIG_IGN
  if (sig == kSigAbrt) hle_abort();
  XBEM_WARN("ucrt", "raise({}) with default action -> terminating", sig);
  proc().exit_process(3);
}

// ---------------------------------------------------------------- threads

uint64_t XBEM_GUEST_ABI hle__beginthreadex(void*, UINT stack, uint64_t start, void* arg, UINT flags, UINT* tid) {
  auto t = proc().create_thread(start, reinterpret_cast<uint64_t>(arg), stack, flags & 0x4 /*CREATE_SUSPENDED*/);
  if (!t) {
    set_errno(11);  // EAGAIN
    return 0;
  }
  if (tid) *tid = t->id;
  return proc().handles().insert(t);
}

[[noreturn]] void XBEM_GUEST_ABI hle__endthreadex(UINT code) { exit_guest_thread(code); }

struct BeginThreadArgs {
  uint64_t fn;
  void* arg;
};

DWORD XBEM_GUEST_ABI beginthread_trampoline(void* raw) {
  const BeginThreadArgs a = *static_cast<BeginThreadArgs*>(raw);
  delete static_cast<BeginThreadArgs*>(raw);
  reinterpret_cast<void(XBEM_GUEST_ABI*)(void*)>(a.fn)(a.arg);
  return 0;
}

uint64_t XBEM_GUEST_ABI hle__beginthread(uint64_t fn, UINT stack, void* arg) {
  auto* a = new BeginThreadArgs{fn, arg};
  auto t = proc().create_thread(reinterpret_cast<uint64_t>(&beginthread_trampoline), reinterpret_cast<uint64_t>(a),
                                stack, false);
  if (!t) {
    delete a;
    set_errno(11);
    return ~0ull;
  }
  return proc().handles().insert(t);
}

[[noreturn]] void XBEM_GUEST_ABI hle__endthread() { exit_guest_thread(0); }

int32_t XBEM_GUEST_ABI hle__getpid() { return static_cast<int32_t>(proc().process_id()); }
int32_t XBEM_GUEST_ABI hle_system(const char*) {
  set_errno(crt::kENOENT);
  return -1;
}

// ------------------------------------------------------------------- heap

void* XBEM_GUEST_ABI hle_malloc(SIZE_T size) {
  void* p = heap_alloc(size, false);
  if (!p) set_errno(crt::kENOMEM);
  return p;
}

void XBEM_GUEST_ABI hle_free(void* p) {
  if (p && !heap_free(p)) crt::invalid_parameter("free (pointer not from malloc)");
}

void* XBEM_GUEST_ABI hle_calloc(SIZE_T count, SIZE_T size) {
  if (size && count > ~SIZE_T(0) / size) {
    set_errno(crt::kENOMEM);
    return nullptr;
  }
  void* p = heap_alloc(count * size, true);
  if (!p) set_errno(crt::kENOMEM);
  return p;
}

void* XBEM_GUEST_ABI hle_realloc(void* p, SIZE_T size) {
  if (p && size == 0) {
    heap_free(p);
    return nullptr;
  }
  void* np = heap_realloc(p, size, false);
  if (!np) set_errno(crt::kENOMEM);
  return np;
}

void* XBEM_GUEST_ABI hle__recalloc(void* p, SIZE_T count, SIZE_T size) {
  if (size && count > ~SIZE_T(0) / size) {
    set_errno(crt::kENOMEM);
    return nullptr;
  }
  void* np = heap_realloc(p, count * size, true);
  if (!np) set_errno(crt::kENOMEM);
  return np;
}

SIZE_T XBEM_GUEST_ABI hle__msize(void* p) {
  if (!heap_owns(p)) {
    crt::invalid_parameter("_msize");
    return ~SIZE_T(0);
  }
  return heap_block_size(p);
}

void* XBEM_GUEST_ABI hle__expand(void* p, SIZE_T size) { return (heap_owns(p) && size <= heap_block_size(p)) ? p : nullptr; }

// Aligned blocks: [size][original pointer][...padding...][aligned data]
void* XBEM_GUEST_ABI hle__aligned_malloc(SIZE_T size, SIZE_T alignment) {
  if (alignment == 0 || (alignment & (alignment - 1))) {
    set_errno(crt::kEINVAL);
    return nullptr;
  }
  alignment = std::max<SIZE_T>(alignment, 16);
  auto* raw = static_cast<uint8_t*>(heap_alloc(size + alignment + 16, false));
  if (!raw) {
    set_errno(crt::kENOMEM);
    return nullptr;
  }
  const uint64_t aligned = (reinterpret_cast<uint64_t>(raw) + 16 + alignment - 1) & ~(alignment - 1);
  reinterpret_cast<uint64_t*>(aligned)[-1] = reinterpret_cast<uint64_t>(raw);
  reinterpret_cast<uint64_t*>(aligned)[-2] = size;
  return reinterpret_cast<void*>(aligned);
}

void XBEM_GUEST_ABI hle__aligned_free(void* p) {
  if (p) heap_free(reinterpret_cast<void*>(reinterpret_cast<uint64_t*>(p)[-1]));
}

SIZE_T XBEM_GUEST_ABI hle__aligned_msize(void* p, SIZE_T, SIZE_T) { return p ? reinterpret_cast<uint64_t*>(p)[-2] : ~SIZE_T(0); }

void* XBEM_GUEST_ABI hle__aligned_realloc(void* p, SIZE_T size, SIZE_T alignment) {
  if (!p) return hle__aligned_malloc(size, alignment);
  if (size == 0) {
    hle__aligned_free(p);
    return nullptr;
  }
  void* np = hle__aligned_malloc(size, alignment);
  if (!np) return nullptr;
  std::memcpy(np, p, std::min<SIZE_T>(size, reinterpret_cast<uint64_t*>(p)[-2]));
  hle__aligned_free(p);
  return np;
}

void* XBEM_GUEST_ABI hle__aligned_recalloc(void* p, SIZE_T count, SIZE_T size, SIZE_T alignment) {
  const SIZE_T total = count * size;
  const SIZE_T old = p ? reinterpret_cast<uint64_t*>(p)[-2] : 0;
  void* np = hle__aligned_realloc(p, total, alignment);
  if (np && total > old) std::memset(static_cast<uint8_t*>(np) + old, 0, total - old);
  return np;
}

std::atomic<uint64_t> g_new_handler{0};
using NewHandler = int32_t(XBEM_GUEST_ABI*)(SIZE_T);

int32_t XBEM_GUEST_ABI hle__callnewh(SIZE_T size) {
  const uint64_t h = g_new_handler.load();
  return h && reinterpret_cast<NewHandler>(h)(size) ? 1 : 0;
}
uint64_t XBEM_GUEST_ABI hle__query_new_handler() { return g_new_handler.load(); }
uint64_t XBEM_GUEST_ABI hle__set_new_handler(uint64_t handler) { return g_new_handler.exchange(handler); }

void* XBEM_GUEST_ABI hle__aligned_offset_malloc(SIZE_T size, SIZE_T alignment, SIZE_T offset) {
  if (alignment == 0 || (alignment & (alignment - 1)) || (offset >= size && size != 0)) {
    set_errno(crt::kEINVAL);
    return nullptr;
  }
  alignment = std::max<SIZE_T>(alignment, 16);
  auto* raw = static_cast<uint8_t*>(heap_alloc(size + alignment + 16, false));
  if (!raw) {
    set_errno(crt::kENOMEM);
    return nullptr;
  }
  // (p + offset) is aligned; the two header words live right below p.
  const uint64_t p = ((reinterpret_cast<uint64_t>(raw) + 16 + offset + alignment - 1) & ~(alignment - 1)) - offset;
  const uint64_t raw_value = reinterpret_cast<uint64_t>(raw), size_value = size;
  std::memcpy(reinterpret_cast<void*>(p - 8), &raw_value, 8);
  std::memcpy(reinterpret_cast<void*>(p - 16), &size_value, 8);
  return reinterpret_cast<void*>(p);
}

void* XBEM_GUEST_ABI hle__aligned_offset_realloc(void* p, SIZE_T size, SIZE_T alignment, SIZE_T offset) {
  if (!p) return hle__aligned_offset_malloc(size, alignment, offset);
  if (size == 0) {
    hle__aligned_free(p);
    return nullptr;
  }
  void* np = hle__aligned_offset_malloc(size, alignment, offset);
  if (!np) return nullptr;
  uint64_t old;
  std::memcpy(&old, static_cast<uint8_t*>(p) - 16, 8);
  std::memcpy(np, p, std::min<SIZE_T>(size, old));
  hle__aligned_free(p);
  return np;
}

[[noreturn]] void XBEM_GUEST_ABI hle__invoke_watson(const char16_t* expr, const char16_t* func, const char16_t* file,
                                                    UINT line, uint64_t) {
  XBEM_ERROR("ucrt", "_invoke_watson: fatal invalid parameter{}{}{} - terminating",
             expr ? " in '" + narrow(expr) + "'" : std::string(), func ? " (" + narrow(func) + ")" : std::string(),
             file ? std::format(" at {}:{}", narrow(file), line) : std::string());
  flush_all();
  proc().exit_process(0xC0000409);  // STATUS_STACK_BUFFER_OVERRUN (fast fail), as on Windows
}

[[noreturn]] void XBEM_GUEST_ABI hle__wassert(const char16_t* message, const char16_t* file, UINT line) {
  XBEM_ERROR("ucrt", "Assertion failed: {}, file {}, line {}", narrow(message), narrow(file), line);
  flush_all();
  proc().exit_process(3);
}
[[noreturn]] void XBEM_GUEST_ABI hle__assert(const char* message, const char* file, UINT line) {
  XBEM_ERROR("ucrt", "Assertion failed: {}, file {}, line {}", narrow(message), narrow(file), line);
  flush_all();
  proc().exit_process(3);
}
int32_t g_new_mode = 0;
int32_t XBEM_GUEST_ABI hle__set_new_mode(int32_t m) { return std::exchange(g_new_mode, m); }
int32_t XBEM_GUEST_ABI hle__query_new_mode() { return g_new_mode; }
int32_t XBEM_GUEST_ABI hle__heapchk() { return -2; }  // _HEAPOK
int32_t XBEM_GUEST_ABI hle__heapmin() { return 0; }
uint64_t XBEM_GUEST_ABI hle__get_heap_handle() { return default_process_heap(); }

}  // namespace

void add_ucrt_runtime_exports(std::vector<ExportDef>& out) {
  out.insert(out.end(), {
      XBEM_HLE(_errno), XBEM_HLE(__doserrno), XBEM_HLE(_get_errno), XBEM_HLE(_set_errno),
      XBEM_HLE(_invalid_parameter_noinfo), XBEM_HLE(_invalid_parameter_noinfo_noreturn), XBEM_HLE(_invalid_parameter),
      XBEM_HLE(_set_invalid_parameter_handler), XBEM_HLE(_get_invalid_parameter_handler),
      XBEM_HLE(_set_thread_local_invalid_parameter_handler), XBEM_HLE(_get_thread_local_invalid_parameter_handler),
      XBEM_HLE(_initialize_onexit_table), XBEM_HLE(_register_onexit_function), XBEM_HLE(_execute_onexit_table),
      XBEM_HLE(atexit), XBEM_HLE(_crt_atexit), XBEM_HLE(_crt_at_quick_exit), XBEM_HLE(at_quick_exit),
      XBEM_HLE(_onexit), XBEM_HLE(exit), XBEM_HLE(_exit), XBEM_HLE(_Exit), XBEM_HLE(quick_exit), XBEM_HLE(_cexit),
      XBEM_HLE(_c_exit), XBEM_HLE(abort), XBEM_HLE(terminate), XBEM_HLE(_set_abort_behavior), XBEM_HLE(_initterm),
      XBEM_HLE(_initterm_e), XBEM_HLE(_seh_filter_dll), XBEM_HLE(_seh_filter_exe),
      XBEM_HLE(_register_thread_local_exe_atexit_callback), XBEM_HLE(_configure_narrow_argv),
      XBEM_HLE(_configure_wide_argv), XBEM_HLE(_initialize_narrow_environment),
      XBEM_HLE(_initialize_wide_environment), XBEM_HLE(__p___argc), XBEM_HLE(__p___argv), XBEM_HLE(__p___wargv),
      XBEM_HLE(__p__acmdln), XBEM_HLE(__p__wcmdln), XBEM_HLE(__p__environ), XBEM_HLE(__p__wenviron),
      XBEM_HLE(_get_initial_narrow_environment), XBEM_HLE(_get_initial_wide_environment), XBEM_HLE(__p__pgmptr),
      XBEM_HLE(__p__wpgmptr), XBEM_HLE(_get_pgmptr), XBEM_HLE(_get_wpgmptr),
      XBEM_HLE(_get_narrow_winmain_command_line), XBEM_HLE(_get_wide_winmain_command_line),
      XBEM_HLE(__getmainargs), XBEM_HLE(__wgetmainargs), XBEM_HLE(_set_app_type), XBEM_HLE(__set_app_type),
      XBEM_HLE(__setusermatherr), XBEM_HLE(_set_fmode), XBEM_HLE(_get_fmode), XBEM_HLE(__p__fmode),
      XBEM_HLE(__p__commode), XBEM_HLE(signal), XBEM_HLE(raise), XBEM_HLE(_beginthreadex), XBEM_HLE(_endthreadex),
      XBEM_HLE(_beginthread), XBEM_HLE(_endthread), XBEM_HLE(_getpid), XBEM_HLE(system), XBEM_HLE(malloc),
      XBEM_HLE(free), XBEM_HLE(calloc), XBEM_HLE(realloc), XBEM_HLE(_recalloc), XBEM_HLE(_msize), XBEM_HLE(_expand),
      XBEM_HLE(_aligned_malloc), XBEM_HLE(_aligned_free), XBEM_HLE(_aligned_msize), XBEM_HLE(_aligned_realloc),
      XBEM_HLE(_aligned_recalloc), XBEM_HLE(_callnewh), XBEM_HLE(_query_new_handler), XBEM_HLE(_set_new_mode),
      XBEM_HLE(_query_new_mode), XBEM_HLE(_heapchk), XBEM_HLE(_heapmin), XBEM_HLE(_get_heap_handle),
      XBEM_HLE(_set_new_handler), XBEM_HLE(_aligned_offset_malloc), XBEM_HLE(_aligned_offset_realloc),
      XBEM_HLE(_invoke_watson), XBEM_HLE(_wassert), XBEM_HLE(_assert),
      // UCRT also exports the allocator under *_base names.
      ExportDef{"_malloc_base", reinterpret_cast<void*>(&hle_malloc)},
      ExportDef{"_free_base", reinterpret_cast<void*>(&hle_free)},
      ExportDef{"_calloc_base", reinterpret_cast<void*>(&hle_calloc)},
      ExportDef{"_realloc_base", reinterpret_cast<void*>(&hle_realloc)},
      ExportDef{"_msize_base", reinterpret_cast<void*>(&hle__msize)},
      ExportDef{"_recalloc_base", reinterpret_cast<void*>(&hle__recalloc)},
  });
}

ModuleDef make_ucrt_module() {
  ModuleDef m;
  m.name = "ucrtbase.dll";
  // msvcrt.dll is served from the same implementation so MinGW-built programs work too.
  m.aliases = {"msvcrt.dll"};
  m.description = "C runtime (UCRT, api-ms-win-crt-*): startup, heap, strings, stdio, math, time, files";
  add_ucrt_runtime_exports(m.exports);
  add_ucrt_string_exports(m.exports);
  add_ucrt_stdio_exports(m.exports);
  add_ucrt_math_exports(m.exports);
  add_ucrt_misc_exports(m.exports);
  return m;
}

}  // namespace xbem::hle
