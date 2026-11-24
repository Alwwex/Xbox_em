#include "runtime/process.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>

#include "common/log.h"
#include "common/strings.h"
#include "hle/guest_types.h"
#include "hle/heap.h"
#include "hle/probe_com.h"
#include "hle/registry.h"
#include "platform/crash.h"
#include "platform/memory.h"
#include "runtime/seh.h"

namespace xbem::rt {

namespace fs = std::filesystem;

namespace {
Process* g_process = nullptr;
thread_local GuestThread* t_current = nullptr;
thread_local uint32_t t_host_last_error = 0;  // for HLE calls on non-guest threads
std::atomic<bool> g_exiting{false};

template <class T>
T& field(uint8_t* base, size_t offset) {
  return *reinterpret_cast<T*>(base + offset);
}

void describe_address(uint64_t address, char* out, size_t out_size) {
  out[0] = 0;
  Process* p = Process::current_or_null();
  if (!p) return;
  if (p->loader().describe(address, out, out_size)) return;
  if (p->trampolines().contains(address)) std::snprintf(out, out_size, "<xbem import stub>");
}

// Crash reports and thread dumps: recent emulated-function and probe calls.
void print_thread_context(void (*write)(const char*)) {
  TrampolinePool::print_call_history(write);
  hle::print_probe_history(write);
}

// Allocations, plus the emulator's probe COM objects (a title storing or
// reading through one shows up in crash reports).
void describe_memory_for_reports(uint64_t address, char* out, size_t out_size) {
  out[0] = 0;
  if (hle::describe_virtual_memory(address, out, out_size)) return;
  uint64_t magic = 0;
  if ((address & 7) == 0 && platform::safe_read(address + 8, &magic, 8) && magic == hle::kProbeMagic) {
    const auto* o = reinterpret_cast<const hle::ProbeObject*>(address);
    std::snprintf(out, out_size, "emulator probe object \"%s\"", o->name.c_str());
  }
}

// For thread dumps: what the call instruction ending at `ret` calls. Handles
// "call [rip+disp32]" (imports through the IAT) and "call rel32".
void describe_call_site(uint64_t ret, char* out, size_t out_size) {
  out[0] = 0;
  Process* p = Process::current_or_null();
  if (!p) return;
  uint8_t before[6];
  if (ret < 0x10006 || !platform::safe_read(ret - 6, before, 6)) return;  // may sit at an image's first byte
  const uint8_t* code = before + 6;
  uint64_t target = 0;
  if (code[-6] == 0xFF && code[-5] == 0x15) {
    int32_t disp;
    std::memcpy(&disp, code - 4, 4);
    const uint64_t slot = ret + static_cast<int64_t>(disp);
    if (!p->loader().describe(slot, out, out_size)) return;  // the IAT lives inside the module
    if (!platform::safe_read(slot, &target, 8)) return;
  } else if (code[-5] == 0xE8) {
    int32_t rel;
    std::memcpy(&rel, code - 4, 4);
    target = ret + static_cast<int64_t>(rel);
  } else {
    return;
  }
  out[0] = 0;
  if (const std::string* name = hle::Registry::instance().name_of(target)) {
    std::snprintf(out, out_size, "-> calls %s", name->c_str());
  } else if (const TracedImport* t = p->trampolines().find_thunk(target)) {
    std::snprintf(out, out_size, "-> calls %s", t->name.c_str());
  } else if (const UnimplementedImport* imp = p->trampolines().find_by_stub(target)) {
    std::snprintf(out, out_size, "-> calls UNIMPLEMENTED %s!%s", imp->dll.c_str(), imp->function.c_str());
  } else {
    char where[200];
    if (p->loader().describe(target, where, sizeof where)) std::snprintf(out, out_size, "-> calls %s", where);
  }
}

// The function a code address belongs to, following chained unwind info to
// the primary entry (the function's real start). Returns its start RVA.
bool primary_function_of(uint64_t pc, uint64_t* begin_rva, uint64_t* end_rva, uint8_t* frame_register) {
  uint64_t base = 0;
  const seh::RuntimeFunction* fn = seh::lookup_function_entry(pc, &base);
  if (!fn) return false;
  *end_rva = fn->EndAddress;
  *frame_register = 0;
  for (int depth = 0; depth < 32; ++depth) {
    uint8_t head[4];
    if (!platform::safe_read(base + fn->UnwindData, head, 4)) break;
    if (depth == 0) *frame_register = head[3] & 0x0F;
    if (!((head[0] >> 3) & seh::kUnwFlagChainInfo)) break;
    const uint64_t chained = base + fn->UnwindData + 4 + ((head[2] + 1u) & ~1u) * 2;
    uint32_t words[3];
    if (!platform::safe_read(chained, words, sizeof words)) break;
    static thread_local seh::RuntimeFunction primary;
    primary = {words[0], words[1], words[2]};
    fn = &primary;
  }
  *begin_rva = fn->BeginAddress;
  return true;
}

// Crash reports: the faulting thread's frames, found with the title's unwind
// data, each with the start of its function (for objdump --start-address).
void print_unwind_backtrace(const uint64_t gpr[16], uint64_t rip, uint64_t stack_low, uint64_t stack_high,
                            void (*write)(const char*)) {
  alignas(16) seh::Context c{};
  std::memcpy(c.Gpr, gpr, sizeof c.Gpr);
  c.Rip = rip;
  seh::DispatcherContext dc{};
  char line[640], where[256], call[256];
  write("Backtrace (from the title's unwind data; fn = start of the function):\n");
  for (int frame = 0; frame < 48 && c.Rip; ++frame) {
    where[0] = 0;
    describe_address(c.Rip, where, sizeof where);
    if (!where[0]) {
      std::snprintf(line, sizeof line, "  #%-2d %016llx  (not in a loaded module - walk ends)\n", frame,
                    static_cast<unsigned long long>(c.Rip));
      write(line);
      return;
    }
    // A return address can sit just past its function (a call as the last
    // instruction): attribute it to the byte before.
    const uint64_t lookup_pc = frame ? c.Rip - 1 : c.Rip;
    uint64_t begin = 0, end = 0;
    uint8_t frame_reg = 0;
    char fn_text[96] = "no unwind data (leaf function)";
    if (primary_function_of(lookup_pc, &begin, &end, &frame_reg)) {
      Process* p = Process::current_or_null();
      const rt::LoadedModule* m = p ? p->loader().find_containing(c.Rip) : nullptr;
      const uint64_t rva = m ? c.Rip - m->handle() : 0;
      std::snprintf(fn_text, sizeof fn_text, "fn 0x%llx +0x%llx", static_cast<unsigned long long>(begin),
                    static_cast<unsigned long long>(rva - begin));
    }
    call[0] = 0;
    if (frame) describe_call_site(c.Rip, call, sizeof call);
    std::snprintf(line, sizeof line, "  #%-2d %s  [%s]%s%s\n", frame, where, fn_text, call[0] ? "  " : "", call);
    write(line);
    if (!seh::is_guest_code(c.Rip)) return;  // host or HLE code: its frames have no Windows unwind data
    const uint64_t rsp = c.Gpr[seh::kRsp];
    const auto on_stack = [&](uint64_t v) { return v >= stack_low && v < stack_high; };
    if (!on_stack(rsp) || (frame_reg && !on_stack(c.Gpr[frame_reg]))) {
      write("  (stack or frame pointer outside the thread's stack - walk ends)\n");
      return;
    }
    if (!seh::unwind_one_frame(seh::kUnwFlagNHandler, dc, c)) return;
    if (c.Gpr[seh::kRsp] <= rsp || c.Gpr[seh::kRsp] > stack_high) {
      write("  (unwind did not move up the stack - walk ends)\n");
      return;
    }
  }
}

std::string quote_arg(const std::string& a) {
  if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) return a;
  std::string out = "\"";
  for (char c : a) {
    if (c == '"') out += '\\';
    out += c;
  }
  return out + "\"";
}
}  // namespace

GuestThread* current_thread() { return t_current; }

void set_guest_last_error(uint32_t error) {
  if (GuestThread* t = t_current; t && t->teb) field<uint32_t>(t->teb, teb::kLastError) = error;
  else t_host_last_error = error;
}

uint32_t guest_last_error() {
  if (GuestThread* t = t_current; t && t->teb) return field<uint32_t>(t->teb, teb::kLastError);
  return t_host_last_error;
}

Process::Process(ProcessOptions options) : options_(std::move(options)) {
  g_process = this;
  process_id_ = platform::host_process_id() * 4;
  tls_slots_.assign(teb::kMinimumSlots + teb::kExpansionSlots, false);
  fls_used_.assign(4080, false);
  fls_callbacks_.assign(4080, 0);
}

Process::~Process() {
  if (g_process == this) g_process = nullptr;
}

Process& Process::current() { return *g_process; }
Process* Process::current_or_null() { return g_process; }

int Process::run() {
  std::string reason;
  if (!platform::guest_execution_supported(&reason)) {
    XBEM_ERROR("process", "cannot execute guest code on this host: {}", reason);
    return 126;
  }

  layout_ = package::scan_package(options_.target);
  if (!layout_.missing_executable.empty()) {
    XBEM_ERROR("process", "MicrosoftGame.config names '{}' as the game executable, but that file is not in {}. "
               "The build is incomplete - the main executable is missing.",
               layout_.missing_executable, layout_.root.string());
    return 2;
  }
  if (layout_.main_executable.empty()) {
    XBEM_ERROR("process", "could not determine the game executable in {} (no MicrosoftGame.config "
               "and not exactly one .exe at the top level) - pass the .exe directly",
               layout_.root.string());
    return 2;
  }
  if (!layout_.config_error.empty()) XBEM_WARN("package", "MicrosoftGame.config: {}", layout_.config_error);
  if (layout_.config)
    XBEM_INFO("package", "title: {} ({}), version {}", layout_.config->display_name, layout_.config->identity_name,
              layout_.config->version);

  // File system: package read-only on G:, writable scratch on T:.
  fs::path scratch = options_.scratch_dir;
  if (scratch.empty()) {
    const std::string id = layout_.config && !layout_.config->identity_name.empty()
                               ? layout_.config->identity_name
                               : layout_.main_executable.stem().string();
    scratch = fs::temp_directory_path() / "xbem" / id;
  }
  std::error_code ec;
  fs::create_directories(scratch, ec);
  vfs_.mount('G', layout_.root, true);
  vfs_.mount('T', scratch, false);
  for (const auto& [drive, path] : options_.extra_mounts) vfs_.mount(drive, path, false);
  vfs_.set_default_drive('G');

  std::vector<fs::path> dirs{layout_.main_executable.parent_path()};
  if (fs::absolute(layout_.root) != fs::absolute(dirs.front())) dirs.push_back(layout_.root);
  loader_ = std::make_unique<ModuleLoader>(dirs, trampolines_);
  TrampolinePool::set_strict(options_.strict);
  TrampolinePool::set_tracing(options_.trace_hle);
  if (options_.trace_hle) XBEM_INFO("process", "--trace-hle: emulated-function calls are traced");

  LoadedModule* main = nullptr;
  try {
    main = &loader_->load_main(layout_.main_executable);
  } catch (const std::exception& e) {
    XBEM_ERROR("loader", "failed to load {}: {}", layout_.main_executable.string(), e.what());
    return 3;
  }
  if (!main->entry) {
    XBEM_ERROR("loader", "{} has no entry point", main->name);
    return 3;
  }

  peb_ = static_cast<uint8_t*>(platform::map_anywhere(peb::kSize, platform::page_size(), platform::Prot::RW));
  field<uint64_t>(peb_, peb::kImageBase) = main->handle();
  field<uint64_t>(peb_, peb::kProcessHeap) = hle::default_process_heap();

  const std::string exe_guest = vfs_.to_guest(layout_.main_executable).value_or(main->name);
  command_line_a_ = quote_arg(exe_guest);
  for (const std::string& a : options_.guest_args) command_line_a_ += " " + quote_arg(a);
  command_line_w_ = utf8_to_utf16(command_line_a_);

  platform::install_crash_handler(&describe_address);
  platform::install_thread_dump(&describe_address, &describe_call_site);
  platform::set_memory_describer(&describe_memory_for_reports);
  platform::set_continue_after_breakpoint(options_.continue_after_breakpoint);
  platform::set_thread_context_printer(&print_thread_context);
  platform::set_backtrace_printer(&print_unwind_backtrace);

  const uint64_t reserve = main->image->optional_header().SizeOfStackReserve;
  // Generous: host pages are only used when touched, and console titles can
  // use large frames early (the console may give the main thread more than
  // the header asks for). 16 MiB minimum plus room for host code.
  const size_t stack = static_cast<size_t>(std::max<uint64_t>(reserve, 16u << 20)) + (1u << 20);
  XBEM_DEBUG("process", "main thread stack {:#x} (image reserve {:#x})", stack, reserve);

  std::shared_ptr<GuestThread> t = std::make_shared<GuestThread>();
  t->is_main = true;
  t->start_routine = main->entry;
  t->parameter = reinterpret_cast<uint64_t>(peb_);
  t->stack_size = stack;
  {
    std::lock_guard lock(threads_mutex_);
    t->id = next_thread_id_;
    next_thread_id_ += 4;
    threads_.push_back(t);
  }
  t->keep_alive = t;
  handles_.insert(t);  // the main thread handle, like Windows
  XBEM_INFO("process", "starting {} (entry {:#x}, device type {})", main->name, main->entry,
            static_cast<uint32_t>(options_.device));
  if (!platform::start_thread(stack, &Process::thread_trampoline, t.get(), t->native)) {
    XBEM_ERROR("process", "cannot create the main guest thread");
    return 4;
  }
  platform::join_thread(t->native);
  exit_process(t->exit_code);
}

void Process::exit_process(uint32_t code) {
  if (g_exiting.exchange(true)) {
    // Another thread is already tearing the process down.
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  print_summary();
  XBEM_INFO("process", "guest exited with code {} ({:#x})", static_cast<int32_t>(code), code);
  std::fflush(nullptr);
  std::_Exit(static_cast<int>(code & 0xFF));
}

void Process::print_summary() {
  auto called = trampolines_.called_imports();
  if (called.empty()) return;
  std::sort(called.begin(), called.end(),
            [](const UnimplementedImport* a, const UnimplementedImport* b) { return a->calls.load() > b->calls.load(); });
  XBEM_WARN("summary", "{} unimplemented imports were called (implement these next):", called.size());
  for (const UnimplementedImport* u : called) XBEM_WARN("summary", "  {:>8} x  {}!{}", u->calls.load(), u->dll, u->function);
}

void Process::thread_trampoline(void* arg) {
  auto* t = static_cast<GuestThread*>(arg);
  Process::current().thread_main(*t);
}

bool Process::setup_thread_environment(GuestThread& t) {
  t.teb = static_cast<uint8_t*>(platform::map_anywhere(teb::kSize, platform::page_size(), platform::Prot::RW));
  t.tls_array = static_cast<void**>(std::calloc(ModuleLoader::kMaxTlsModules, sizeof(void*)));
  if (!t.teb || !t.tls_array) return false;

  uintptr_t low = 0, high = 0;
  platform::current_stack_bounds(low, high);
  field<uint64_t>(t.teb, teb::kStackBase) = high;
  field<uint64_t>(t.teb, teb::kStackLimit) = low;
  field<uint64_t>(t.teb, 0x1478) = low;  // DeallocationStack (read by stack-size checks)
  field<uint64_t>(t.teb, teb::kSelf) = reinterpret_cast<uint64_t>(t.teb);
  field<uint64_t>(t.teb, teb::kClientIdProcess) = process_id_;
  field<uint64_t>(t.teb, teb::kClientIdThread) = t.id;
  field<uint64_t>(t.teb, teb::kThreadLocalStoragePointer) = reinterpret_cast<uint64_t>(t.tls_array);
  field<uint64_t>(t.teb, teb::kPeb) = reinterpret_cast<uint64_t>(peb_);

  allocate_tls_blocks(t, loader_->all_modules());
  if (!platform::set_guest_teb(t.teb)) {
    XBEM_ERROR("process", "cannot point GS at the emulated TEB");
    return false;
  }
  return true;
}

void Process::allocate_tls_blocks(GuestThread& t, const std::vector<LoadedModule*>& modules) {
  for (LoadedModule* m : modules) {
    if (m->tls_index < 0 || t.tls_array[m->tls_index]) continue;
    const size_t align = m->tls_alignment;
    const size_t template_size = static_cast<size_t>(m->tls_template_end - m->tls_template_start);
    const size_t size = platform::align_up(std::max<size_t>(m->tls_block_size(), 16), align);
    auto* block = static_cast<uint8_t*>(::operator new(size, std::align_val_t(align)));
    std::memset(block, 0, size);
    if (template_size) std::memcpy(block, reinterpret_cast<const void*>(m->tls_template_start), template_size);
    t.tls_array[m->tls_index] = block;
  }
}

void Process::notify_modules(uint32_t reason, const std::vector<LoadedModule*>& modules, bool static_load) {
  const bool thread_event = reason == win::DLL_THREAD_ATTACH || reason == win::DLL_THREAD_DETACH;
  void* reserved = static_load ? reinterpret_cast<void*>(1) : nullptr;
  for (LoadedModule* m : modules) {
    if (m->hle) continue;
    for (uint64_t cb : m->tls_callbacks) {
      XBEM_TRACE("process", "{}: TLS callback {:#x} reason {}", m->name, cb, reason);
      reinterpret_cast<win::GuestTlsCallback>(cb)(m->handle(), reason, reserved);
    }
    if (!m->is_dll || !m->entry || (thread_event && m->thread_calls_disabled)) continue;
    XBEM_DEBUG("process", "{}: DllMain(reason {})", m->name, reason);
    const win::BOOL ok = reinterpret_cast<win::GuestDllMain>(m->entry)(m->handle(), reason, reserved);
    if (reason == win::DLL_PROCESS_ATTACH && !ok) XBEM_ERROR("process", "{}: DllMain(PROCESS_ATTACH) failed", m->name);
  }
}

void Process::initialize_pending_modules() {
  std::vector<LoadedModule*> mods = loader_->take_uninitialized();
  if (mods.empty()) return;
  {
    std::lock_guard lock(threads_mutex_);
    for (auto& t : threads_)
      if (t->tls_array) allocate_tls_blocks(*t, mods);
  }
  // lpReserved is non-NULL for modules loaded with the process (static loads).
  const bool static_load = !initial_init_done_;
  initial_init_done_ = true;
  notify_modules(win::DLL_PROCESS_ATTACH, mods, static_load);
  for (LoadedModule* m : mods) m->attached = true;
}

void Process::run_fls_callbacks(GuestThread& t) {
  std::vector<std::pair<uint64_t, uint64_t>> calls;
  {
    std::lock_guard lock(slots_mutex_);
    for (size_t i = 0; i < t.fls_values.size(); ++i)
      if (fls_used_[i] && fls_callbacks_[i] && t.fls_values[i]) calls.emplace_back(fls_callbacks_[i], t.fls_values[i]);
  }
  for (auto [cb, value] : calls) reinterpret_cast<win::GuestFlsCallback>(cb)(reinterpret_cast<void*>(value));
}

void Process::thread_main(GuestThread& t) {
  t_current = &t;
  platform::setup_thread_signal_stack();
  platform::set_current_thread_name(t.is_main ? "guest-main" : "guest-" + std::to_string(t.id));
  if (!setup_thread_environment(t)) {
    std::lock_guard lock(sync_mutex());
    t.finished = true;
    t.exit_code = 1;
    return;
  }
  {
    uint64_t stack_low = 0, stack_high = 0;
    std::memcpy(&stack_low, t.teb + teb::kStackLimit, 8);
    std::memcpy(&stack_high, t.teb + teb::kStackBase, 8);
    platform::register_dump_thread(t.id, stack_low, stack_high);
  }
  {
    std::unique_lock lock(sync_mutex());
    sync_cv().wait(lock, [&] { return t.suspend_count <= 0; });
  }

  if (setjmp(t.exit_jump) == 0) {
    t.exit_jump_valid = true;
    if (t.is_main) {
      initialize_pending_modules();
      const uint32_t code = reinterpret_cast<win::GuestEntry>(t.start_routine)(reinterpret_cast<void*>(t.parameter));
      XBEM_DEBUG("process", "entry point returned {}", code);
      exit_process(code);  // the CRT would call ExitProcess here as well
    } else {
      notify_modules(win::DLL_THREAD_ATTACH, loader_->initialized_modules(), false);
      const uint32_t code =
          reinterpret_cast<win::GuestThreadProc>(t.start_routine)(reinterpret_cast<void*>(t.parameter));
      std::lock_guard lock(sync_mutex());
      t.exit_code = code;
    }
  }
  // Normal return or ExitThread() (longjmp) lands here.
  t.exit_jump_valid = false;
  if (!t.is_main) {
    auto mods = loader_->initialized_modules();
    std::reverse(mods.begin(), mods.end());
    notify_modules(win::DLL_THREAD_DETACH, mods, false);
  }
  run_fls_callbacks(t);
  platform::unregister_dump_thread();

  {
    std::lock_guard lock(sync_mutex());
    t.finished = true;
  }
  sync_cv().notify_all();
  {
    std::lock_guard lock(threads_mutex_);
    threads_.erase(std::remove_if(threads_.begin(), threads_.end(), [&](const auto& p) { return p.get() == &t; }),
                   threads_.end());
  }
  if (t.is_main) {
    XBEM_WARN("process", "main thread called ExitThread; terminating the process");
    exit_process(t.exit_code);
  }
  t_current = nullptr;
  auto keep = std::move(t.keep_alive);  // may destroy `t` when `keep` goes out of scope
}

std::shared_ptr<GuestThread> Process::create_thread(uint64_t start, uint64_t param, size_t stack_size,
                                                    bool suspended) {
  auto t = std::make_shared<GuestThread>();
  t->start_routine = start;
  t->parameter = param;
  t->suspend_count = suspended ? 1 : 0;
  // Windows gives a thread at least the image's default reserve (the size
  // argument is normally a commit size). Host pages are only used when touched.
  uint64_t image_reserve = 0;
  if (loader_->main_module() && loader_->main_module()->image)
    image_reserve = loader_->main_module()->image->optional_header().SizeOfStackReserve;
  stack_size = static_cast<size_t>(std::max<uint64_t>({stack_size, image_reserve, 2u << 20}));
  t->stack_size = stack_size + (1u << 20);  // + room for host code
  {
    std::lock_guard lock(threads_mutex_);
    t->id = next_thread_id_;
    next_thread_id_ += 4;
    threads_.push_back(t);
  }
  t->keep_alive = t;
  if (!platform::start_thread(t->stack_size, &Process::thread_trampoline, t.get(), t->native)) {
    std::lock_guard lock(threads_mutex_);
    threads_.pop_back();
    t->keep_alive.reset();
    return nullptr;
  }
  platform::detach_thread(t->native);
  XBEM_DEBUG("process", "created thread {} (start {:#x})", t->id, start);
  return t;
}

int Process::resume_thread(GuestThread& t) {
  int previous;
  {
    std::lock_guard lock(sync_mutex());
    previous = t.suspend_count;
    if (t.suspend_count > 0) --t.suspend_count;
  }
  sync_cv().notify_all();
  return previous;
}

std::shared_ptr<GuestThread> Process::current_thread_shared() {
  GuestThread* self = current_thread();
  std::lock_guard lock(threads_mutex_);
  for (auto& t : threads_)
    if (t.get() == self) return t;
  return nullptr;
}

std::shared_ptr<GuestThread> Process::find_thread(uint32_t id) {
  std::lock_guard lock(threads_mutex_);
  for (auto& t : threads_)
    if (t->id == id) return t;
  return nullptr;
}

size_t Process::live_thread_count() {
  std::lock_guard lock(threads_mutex_);
  return threads_.size();
}

uint32_t Process::tls_alloc() {
  uint32_t index = win::TLS_OUT_OF_INDEXES;
  {
    std::lock_guard lock(slots_mutex_);
    for (uint32_t i = 0; i < tls_slots_.size(); ++i) {
      if (!tls_slots_[i]) {
        tls_slots_[i] = true;
        index = i;
        break;
      }
    }
  }
  if (index == win::TLS_OUT_OF_INDEXES) return index;
  // Windows zeroes a freshly allocated slot in every thread.
  std::lock_guard lock(threads_mutex_);
  for (auto& t : threads_) {
    if (!t->teb) continue;
    if (index < teb::kMinimumSlots) {
      field<uint64_t>(t->teb, teb::kTlsSlots + index * 8) = 0;
    } else if (auto* exp = field<uint64_t*>(t->teb, teb::kTlsExpansionSlots)) {
      exp[index - teb::kMinimumSlots] = 0;
    }
  }
  return index;
}

bool Process::tls_free(uint32_t index) {
  std::lock_guard lock(slots_mutex_);
  if (index >= tls_slots_.size() || !tls_slots_[index]) return false;
  tls_slots_[index] = false;
  return true;
}

uint32_t Process::fls_alloc(uint64_t callback) {
  std::lock_guard lock(slots_mutex_);
  for (uint32_t i = 0; i < fls_used_.size(); ++i) {
    if (fls_used_[i]) continue;
    fls_used_[i] = true;
    fls_callbacks_[i] = callback;
    return i;
  }
  return win::TLS_OUT_OF_INDEXES;
}

bool Process::fls_free(uint32_t index) {
  std::lock_guard lock(slots_mutex_);
  if (index >= fls_used_.size() || !fls_used_[index]) return false;
  fls_used_[index] = false;
  fls_callbacks_[index] = 0;
  return true;
}

}  // namespace xbem::rt
