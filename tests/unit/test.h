#pragma once

// Minimal self-registering test framework (no external dependencies).

#include <sstream>
#include <string>
#include <vector>

namespace xbt {

struct TestCase {
  const char* name;
  void (*fn)();
};

std::vector<TestCase>& registry();
void report_failure(const char* file, int line, const std::string& message);

struct Registrar {
  Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

}  // namespace xbt

#define TEST(name)                                                   \
  static void xbt_test_##name();                                     \
  static ::xbt::Registrar xbt_registrar_##name(#name, &xbt_test_##name); \
  static void xbt_test_##name()

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) ::xbt::report_failure(__FILE__, __LINE__, "CHECK(" #cond ")"); \
  } while (0)

#define CHECK_EQ(a, b)                                                         \
  do {                                                                         \
    const auto& xbt_a = (a);                                                   \
    const auto& xbt_b = (b);                                                   \
    if (!(xbt_a == xbt_b)) {                                                   \
      std::ostringstream xbt_os;                                               \
      xbt_os << "CHECK_EQ(" #a ", " #b "): " << xbt_a << " != " << xbt_b;      \
      ::xbt::report_failure(__FILE__, __LINE__, xbt_os.str());                 \
    }                                                                          \
  } while (0)

#define CHECK_THROWS(expr)                                                              \
  do {                                                                                  \
    bool xbt_threw = false;                                                             \
    try {                                                                               \
      (void)(expr);                                                                     \
    } catch (...) {                                                                     \
      xbt_threw = true;                                                                 \
    }                                                                                   \
    if (!xbt_threw) ::xbt::report_failure(__FILE__, __LINE__, "expected exception: " #expr); \
  } while (0)
