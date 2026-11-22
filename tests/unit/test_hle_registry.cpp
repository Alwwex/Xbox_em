#include <set>

#include "hle/registry.h"
#include "test.h"

using namespace xbem;

TEST(registry_module_lookup) {
  const auto& reg = hle::Registry::instance();
  const auto* kx = reg.find_module("kernelx.dll");
  CHECK(kx != nullptr);
  CHECK(reg.find_module("KERNELX.DLL") == kx);
  CHECK(reg.find_module("kernelx") == kx);
  CHECK(reg.find_module("C:\\Windows\\System32\\kernel32.dll") == kx);
  CHECK(reg.find_module("api-ms-win-core-synch-l1-2-0.dll") == kx);
  CHECK(reg.find_module("api-ms-win-crt-runtime-l1-1-0.dll") == nullptr);  // CRT is not provided by the OS layer
  CHECK(reg.find_module("d3d12_x.dll") == reg.find_module("d3d12_xs.dll"));
  CHECK(reg.find_module("totally-unknown.dll") == nullptr);
}

TEST(registry_export_lookup) {
  const auto& reg = hle::Registry::instance();
  auto l = reg.find("api-ms-win-core-processthreads-l1-1-0.dll", "CreateThread");
  CHECK(l.exp != nullptr && l.exp->address != nullptr);
  CHECK(reg.find("kernelx.dll", "NoSuchExport").exp == nullptr);
  auto d = reg.find("d3d12_xs.dll", "D3D12XboxCreateDevice");
  CHECK(d.exp != nullptr && d.exp->kind == hle::ExportKind::Stub);
  auto x = reg.find("xgameruntime.dll", "XTaskQueueCreate");
  CHECK(x.exp != nullptr && x.exp->kind == hle::ExportKind::Implemented);
}

TEST(registry_has_no_duplicate_exports) {
  for (const auto& m : hle::Registry::instance().modules()) {
    std::set<std::string_view> seen;
    for (const auto& e : m.exports) {
      if (!seen.insert(e.name).second) xbt::report_failure(__FILE__, __LINE__, m.name + ": duplicate " + std::string(e.name));
      CHECK(e.address != nullptr);
    }
  }
}

TEST(registry_normalize) {
  CHECK_EQ(hle::Registry::normalize_dll_name("Foo"), std::string("foo.dll"));
  CHECK_EQ(hle::Registry::normalize_dll_name("bar.EXE"), std::string("bar.exe"));
  CHECK_EQ(hle::Registry::normalize_dll_name("a/b\\Baz.dll"), std::string("baz.dll"));
}
