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
  const auto* crt = reg.find_module("ucrtbase.dll");
  CHECK(crt != nullptr && crt != kx);
  CHECK(reg.find_module("api-ms-win-crt-runtime-l1-1-0.dll") == crt);  // C runtime API sets -> ucrtbase
  CHECK(reg.find_module("msvcrt.dll") == crt);
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

TEST(registry_ordinal_lookup) {
  const auto& reg = hle::Registry::instance();
  auto startup = reg.find_ordinal("WS2_32.dll", 115);
  CHECK(startup.exp != nullptr && startup.exp->name == "WSAStartup");
  CHECK(reg.find_ordinal("ws2_32.dll", 9).exp == reg.find("ws2_32.dll", "htons").exp);
  CHECK(reg.find_ordinal("oleaut32.dll", 6).exp != nullptr);
  CHECK(reg.find_ordinal("ws2_32.dll", 4000).exp == nullptr);
  CHECK(reg.find_ordinal("kernelx.dll", 1).exp == nullptr);  // name-only module
}

TEST(registry_ordinals_unique) {
  for (const auto& m : hle::Registry::instance().modules()) {
    std::set<uint16_t> seen;
    for (const auto& e : m.exports)
      if (e.ordinal && !seen.insert(e.ordinal).second)
        xbt::report_failure(__FILE__, __LINE__, m.name + ": duplicate ordinal " + std::to_string(e.ordinal));
  }
}

TEST(registry_routes_new_modules) {
  const auto& reg = hle::Registry::instance();
  CHECK(reg.find("ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0.dll", "CreateWindowExA").exp != nullptr);
  CHECK(reg.find("ext-ms-win-rtcore-ntuser-message-l1-1-0.dll", "TranslateMessage").exp != nullptr);
  CHECK(reg.find("xmem.dll", "XMemVirtualAlloc").exp != nullptr);
  CHECK(reg.find("WINHTTP.dll", "WinHttpOpen").exp != nullptr);
  CHECK(reg.find("api-ms-win-core-localization-l1-2-0.dll", "FormatMessageW").exp != nullptr);
  CHECK(reg.find("api-ms-win-crt-time-l1-1-0.dll", "_Getdays").exp != nullptr);
  CHECK(reg.find_ordinal("GameInput.dll", 1).exp != nullptr);
  const auto look = reg.find("kernelx.dll", "CreateThread");
  const std::string* name = reg.name_of(reinterpret_cast<uint64_t>(look.exp->address));
  CHECK(name != nullptr && *name == "kernelx.dll!CreateThread");
}
