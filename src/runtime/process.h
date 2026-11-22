#pragma once

#include <csetjmp>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "package/package_layout.h"
#include "platform/thread.h"
#include "runtime/module_loader.h"
#include "runtime/objects.h"
#include "runtime/trampolines.h"
#include "runtime/vfs.h"

namespace xbem::rt {

// XSystemDeviceType values reported to the title (GDK enum).
enum class DeviceType : uint32_t {
  Unknown = 0,
  Pc = 1,
  XboxOne = 2,
  XboxOneS = 3,
  XboxOneX = 4,
  XboxOneXDevkit = 5,
  XboxScarlettLockhart = 6,  // Xbox Series S
  XboxScarlettAnaconda = 7,  // Xbox Series X
  XboxScarlettDevkit = 8,
};

struct ProcessOptions {
  std::filesystem::path target;  // package directory or .exe
  bool strict = false;           // terminate on the first call to an unimplemented import
  DeviceType device = DeviceType::XboxScarlettAnaconda;
  std::filesystem::path scratch_dir;  // host directory for T:\ (created if missing)
  std::vector<std::pair<char, std::filesystem::path>> extra_mounts;
  std::vector<std::string> guest_args;
};

// Offsets inside the x64 TEB / PEB that guest code (and the MSVC runtime)
// accesses directly through the GS segment.
namespace teb {
constexpr size_t kStackBase = 0x08;
constexpr size_t kStackLimit = 0x10;
constexpr size_t kSelf = 0x30;
constexpr size_t kClientIdProcess = 0x40;
constexpr size_t kClientIdThread = 0x48;
constexpr size_t kThreadLocalStoragePointer = 0x58;
constexpr size_t kPeb = 0x60;
constexpr size_t kLastError = 0x68;
constexpr size_t kTlsSlots = 0x1480;           // 64 x 8 bytes
constexpr size_t kTlsExpansionSlots = 0x1780;  // pointer to 1024 x 8 bytes
constexpr size_t kSize = 0x2000;
constexpr uint32_t kMinimumSlots = 64;
constexpr uint32_t kExpansionSlots = 1024;
}  // namespace teb

namespace peb {
constexpr size_t kBeingDebugged = 0x02;
constexpr size_t kImageBase = 0x10;
constexpr size_t kProcessHeap = 0x30;
constexpr size_t kSize = 0x1000;
}  // namespace peb

class GuestThread final : public KernelObject {
 public:
  const char* type_name() const override { return "Thread"; }
  bool waitable() const override { return true; }
  bool is_signaled(uint32_t) const override { return finished; }

  uint32_t id = 0;
  bool is_main = false;
  uint8_t* teb = nullptr;
  void** tls_array = nullptr;
  uint64_t start_routine = 0;
  uint64_t parameter = 0;
  size_t stack_size = 0;
  uint64_t affinity_mask = ~0ull;
  int priority = 0;
  std::vector<uint64_t> fls_values;

  // Guarded by sync_mutex().
  bool finished = false;
  int suspend_count = 0;
  uint32_t exit_code = 259;  // STILL_ACTIVE

  std::jmp_buf exit_jump;
  bool exit_jump_valid = false;
  platform::NativeThread native;
  std::shared_ptr<GuestThread> keep_alive;  // released when the thread finishes
};

// The calling guest thread, or nullptr on host-only threads.
GuestThread* current_thread();

// Last-error value of the calling thread (TEB+0x68).
void set_guest_last_error(uint32_t error);
uint32_t guest_last_error();

class Process {
 public:
  explicit Process(ProcessOptions options);
  ~Process();
  Process(const Process&) = delete;
  Process& operator=(const Process&) = delete;

  static Process& current();
  static Process* current_or_null();

  // Loads the title and runs it. Returns the guest exit code.
  int run();

  [[noreturn]] void exit_process(uint32_t code);

  ModuleLoader& loader() { return *loader_; }
  HandleTable& handles() { return handles_; }
  Vfs& vfs() { return vfs_; }
  TrampolinePool& trampolines() { return trampolines_; }
  const ProcessOptions& options() const { return options_; }
  const package::PackageLayout& layout() const { return layout_; }

  uint8_t* peb() const { return peb_; }
  uint32_t process_id() const { return process_id_; }

  std::shared_ptr<GuestThread> create_thread(uint64_t start, uint64_t param, size_t stack_size, bool suspended);
  // Returns the previous suspend count, or -1 on failure.
  int resume_thread(GuestThread& thread);
  std::shared_ptr<GuestThread> current_thread_shared();
  size_t live_thread_count();

  uint32_t tls_alloc();
  bool tls_free(uint32_t index);

  uint32_t fls_alloc(uint64_t callback);
  bool fls_free(uint32_t index);

  // Runs TLS setup, TLS callbacks and DllMain(DLL_PROCESS_ATTACH) for modules
  // that were loaded but not yet initialised. Must run on a guest thread.
  void initialize_pending_modules();

  const std::u16string& command_line_w() const { return command_line_w_; }
  const std::string& command_line_a() const { return command_line_a_; }

  void print_summary();

 private:
  static void thread_trampoline(void* arg);
  void thread_main(GuestThread& t);
  bool setup_thread_environment(GuestThread& t);
  void allocate_tls_blocks(GuestThread& t, const std::vector<LoadedModule*>& modules);
  void notify_modules(uint32_t reason, const std::vector<LoadedModule*>& modules, bool static_load);
  void run_fls_callbacks(GuestThread& t);

  ProcessOptions options_;
  package::PackageLayout layout_;
  Vfs vfs_;
  HandleTable handles_;
  TrampolinePool trampolines_;
  std::unique_ptr<ModuleLoader> loader_;
  uint8_t* peb_ = nullptr;
  uint32_t process_id_ = 0;
  std::u16string command_line_w_;
  std::string command_line_a_;

  std::mutex threads_mutex_;
  std::vector<std::shared_ptr<GuestThread>> threads_;
  uint32_t next_thread_id_ = 0x104;
  bool initial_init_done_ = false;

  std::mutex slots_mutex_;
  std::vector<bool> tls_slots_;
  std::vector<uint64_t> fls_callbacks_;  // 0 = free slot sentinel handled by fls_used_
  std::vector<bool> fls_used_;
};

}  // namespace xbem::rt
