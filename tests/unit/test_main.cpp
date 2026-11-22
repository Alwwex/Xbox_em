#include <cstdio>
#include <cstring>
#include <exception>

#include "common/log.h"
#include "test.h"

namespace xbt {

namespace {
int g_failures_in_test = 0;
}

std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

void report_failure(const char* file, int line, const std::string& message) {
  ++g_failures_in_test;
  std::fprintf(stderr, "    %s:%d: %s\n", file, line, message.c_str());
}

}  // namespace xbt

int main(int argc, char** argv) {
  xbem::log::set_level(xbem::log::Level::Error);
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int failed = 0, run = 0;
  for (const auto& t : xbt::registry()) {
    if (filter && !std::strstr(t.name, filter)) continue;
    ++run;
    xbt::g_failures_in_test = 0;
    try {
      t.fn();
    } catch (const std::exception& e) {
      xbt::report_failure(__FILE__, __LINE__, std::string("unexpected exception: ") + e.what());
    }
    const bool ok = xbt::g_failures_in_test == 0;
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", t.name);
    failed += ok ? 0 : 1;
  }
  std::printf("\n%d/%d tests passed\n", run - failed, run);
  return failed ? 1 : 0;
}
