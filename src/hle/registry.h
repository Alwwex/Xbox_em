#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace xbem::hle {

enum class ExportKind {
  Implemented,  // behaves like the real function (possibly simplified)
  Stub,         // deliberately reports "not supported" in a way the game can handle
};

struct ExportDef {
  std::string_view name;
  void* address;
  ExportKind kind = ExportKind::Implemented;
};

struct ModuleDef {
  std::string name;                  // canonical, lower-case, e.g. "kernelx.dll"
  std::vector<std::string> aliases;  // other DLL names served by this module
  std::vector<ExportDef> exports;
  std::string description;
};

// All HLE modules known to the emulator. Lookups are ASCII case-insensitive and
// understand Windows API-set names (api-ms-win-core-*), which are redirected to
// the module that implements the kernel functions.
class Registry {
 public:
  static const Registry& instance();

  struct Lookup {
    const ModuleDef* module = nullptr;
    const ExportDef* exp = nullptr;
  };

  const ModuleDef* find_module(std::string_view dll_name) const;
  Lookup find(std::string_view dll_name, std::string_view function) const;
  const std::vector<ModuleDef>& modules() const { return modules_; }

  static std::string normalize_dll_name(std::string_view dll_name);
  static bool is_api_set(std::string_view dll_name);

 private:
  Registry();
  std::vector<ModuleDef> modules_;
  std::unordered_map<std::string, size_t> by_name_;
  std::vector<std::unordered_map<std::string, size_t>> export_index_;
  const ModuleDef* api_set_target_ = nullptr;
  const ModuleDef* crt_target_ = nullptr;
};

// Implemented by the individual module files.
ModuleDef make_kernelx_module();
ModuleDef make_xgameruntime_module();
ModuleDef make_d3d12_module();
ModuleDef make_ucrt_module();

// Pieces of kernelx implemented in separate files.
void add_kernelx_memory_exports(std::vector<ExportDef>& out);
void add_kernelx_sync_exports(std::vector<ExportDef>& out);
void add_kernelx_file_exports(std::vector<ExportDef>& out);
void add_kernelx_seh_exports(std::vector<ExportDef>& out);

// Handle value returned by GetProcessHeap() (also stored in the PEB).
uint64_t default_process_heap();

}  // namespace xbem::hle

// Helpers for module tables: HLE functions are named hle_<ExportName>.
#define XBEM_HLE(fn) ::xbem::hle::ExportDef{#fn, reinterpret_cast<void*>(&hle_##fn)}
#define XBEM_HLE_STUB(fn) \
  ::xbem::hle::ExportDef{#fn, reinterpret_cast<void*>(&hle_##fn), ::xbem::hle::ExportKind::Stub}
