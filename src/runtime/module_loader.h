#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "hle/registry.h"
#include "loader/pe_image.h"
#include "runtime/trampolines.h"

namespace xbem::rt {

// A module in the guest address space: either a PE image we mapped (the game
// executable and the DLLs it ships) or an HLE module implemented by xbem.
struct LoadedModule {
  std::string name;  // lower-case file name, e.g. "game.exe"
  std::filesystem::path host_path;
  std::unique_ptr<pe::PeImage> image;     // null for HLE modules
  const hle::ModuleDef* hle = nullptr;    // non-null for HLE modules
  uint8_t* base = nullptr;
  size_t size = 0;
  uint64_t entry = 0;
  bool is_dll = false;

  // Implicit (static) TLS.
  int tls_index = -1;
  uint64_t tls_template_start = 0;
  uint64_t tls_template_end = 0;
  uint32_t tls_zero_fill = 0;
  uint32_t tls_alignment = 16;
  std::vector<uint64_t> tls_callbacks;

  bool attached = false;  // DLL_PROCESS_ATTACH / TLS callbacks done
  bool thread_calls_disabled = false;

  struct ExportTarget {
    uint64_t address = 0;
    std::string forwarder;
  };
  std::unordered_map<std::string, ExportTarget> exports_by_name;
  std::unordered_map<uint16_t, ExportTarget> exports_by_ordinal;

  uint64_t handle() const { return reinterpret_cast<uint64_t>(base); }
  bool contains(uint64_t a) const { return a >= handle() && a - handle() < size; }
  size_t tls_block_size() const { return static_cast<size_t>(tls_template_end - tls_template_start) + tls_zero_fill; }
};

class ModuleLoader {
 public:
  ModuleLoader(std::vector<std::filesystem::path> search_dirs, TrampolinePool& trampolines);

  LoadedModule& load_main(const std::filesystem::path& exe);

  // Loads (or returns the already loaded) module by DLL name: HLE modules
  // first, then DLLs found in the search directories. nullptr if not found.
  LoadedModule* load_library(std::string_view name);
  LoadedModule* load_library_path(const std::filesystem::path& host_path);

  LoadedModule* find_loaded(std::string_view name);
  LoadedModule* find_by_handle(uint64_t handle);
  LoadedModule* find_containing(uint64_t address);
  LoadedModule* main_module() { return main_; }

  uint64_t get_proc_address(LoadedModule& module, std::string_view name);
  uint64_t get_proc_address(LoadedModule& module, uint16_t ordinal);

  // Modules loaded but not yet initialised, dependencies first.
  std::vector<LoadedModule*> take_uninitialized();
  std::vector<LoadedModule*> initialized_modules();
  std::vector<LoadedModule*> all_modules();

  // Lock-free lookup used by the crash handler.
  bool describe(uint64_t address, char* out, size_t out_size) const;

  static constexpr int kMaxTlsModules = 1024;

 private:
  LoadedModule* load_pe(const std::filesystem::path& path, const std::string& name);
  LoadedModule* load_hle(const hle::ModuleDef& def);
  void map_image(LoadedModule& m);
  void index_exports(LoadedModule& m);
  void resolve_imports(LoadedModule& m);
  void setup_tls(LoadedModule& m);
  void apply_protections(LoadedModule& m);
  uint64_t resolve_export(LoadedModule& m, const LoadedModule::ExportTarget& t, int depth);
  uint64_t get_proc_address_impl(LoadedModule& m, std::string_view name, int depth);
  std::optional<std::filesystem::path> locate(std::string_view file_name) const;
  void publish_range(const LoadedModule& m);

  std::recursive_mutex mutex_;
  std::vector<std::filesystem::path> search_dirs_;
  TrampolinePool& trampolines_;
  uint64_t traced(const hle::Registry::Lookup& look, uint64_t address);
  std::vector<std::unique_ptr<LoadedModule>> modules_;
  std::unordered_map<std::string, LoadedModule*> by_name_;
  std::vector<LoadedModule*> pending_init_;
  LoadedModule* main_ = nullptr;
  int next_tls_index_ = 1;  // 0 is reserved for the executable
  bool exe_tls_assigned_ = false;

  struct Range {
    uint64_t base;
    uint64_t size;
    char name[64];
  };
  std::array<Range, 512> ranges_{};
  std::atomic<size_t> range_count_{0};
};

}  // namespace xbem::rt
