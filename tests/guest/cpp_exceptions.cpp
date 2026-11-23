// Real C++ exceptions (MinGW's SEH-based unwinder from libgcc/libstdc++)
// running on xbem's RaiseException / RtlUnwindEx / RtlVirtualUnwind and its
// C runtime. Built with -nostartfiles, so it performs the CRT start-up steps
// it needs itself.

#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
__declspec(dllimport) void OutputDebugStringA(const char*);
__declspec(dllimport) void* CreateThread(void*, size_t, unsigned long (*)(void*), void*, unsigned long, unsigned long*);
__declspec(dllimport) unsigned long WaitForSingleObject(void*, unsigned long);
__declspec(dllimport) int GetExitCodeThread(void*, unsigned long*);
__declspec(dllimport) int CloseHandle(void*);
void __main();
void _pei386_runtime_relocator();
// Static destructors are never run at process exit in this test; a local
// atexit also avoids MinGW's 32-bit auto-import fixup for msvcrt!atexit.
int atexit(void (*)(void)) { return 0; }
}

static int g_failures;
#define CHECK(c)                                                                  \
  do {                                                                            \
    if (!(c)) {                                                                   \
      char buf[160];                                                              \
      std::snprintf(buf, sizeof buf, "CHECK FAILED line %d: %s", __LINE__, #c);   \
      OutputDebugStringA(buf);                                                    \
      ++g_failures;                                                               \
    }                                                                             \
  } while (0)

static int g_dtors;
struct Tracker {
  ~Tracker() { ++g_dtors; }
};

struct GameError : std::runtime_error {
  int code;
  explicit GameError(int c) : std::runtime_error("game error"), code(c) {}
};

__attribute__((noinline)) static void thrower(int depth) {
  Tracker t;
  if (depth == 0) throw GameError(42);
  thrower(depth - 1);
}

__attribute__((noinline)) static int rethrow_from_catch() {
  try {
    try {
      throw 7;
    } catch (int) {
      throw;  // rethrow the in-flight exception
    }
  } catch (int v) {
    return v;
  }
  return -1;
}

__attribute__((noinline)) static std::string throw_inside_catch() {
  try {
    try {
      throw std::logic_error("first");
    } catch (const std::logic_error&) {
      throw std::string("second");
    }
  } catch (const std::string& s) {
    return s;
  }
  return "none";
}

__attribute__((noinline)) static int catch_all() {
  try {
    thrower(3);
  } catch (...) {
    return 1;
  }
  return 0;
}

__attribute__((noinline)) static int unique_ptr_cleanup() {
  static int deleted = 0;
  struct Res {
    ~Res() { ++deleted; }
  };
  try {
    auto a = std::make_unique<Res>();
    auto b = std::make_unique<Res>();
    throw std::runtime_error("cleanup");
  } catch (const std::exception&) {
  }
  return deleted;
}

static unsigned long worker(void*) {
  try {
    std::vector<std::string> names{"alpha", "beta"};
    names.at(5);
  } catch (const std::out_of_range&) {
    return 5;
  }
  return 6;
}

extern "C" unsigned guest_main(void*) {
  _pei386_runtime_relocator();
  __main();

  // 1. Through several frames with destructors, caught by base-class reference.
  try {
    thrower(5);
  } catch (const std::runtime_error& e) {
    const GameError* ge = dynamic_cast<const GameError*>(&e);
    CHECK(ge && ge->code == 42);
    CHECK(std::string(e.what()) == "game error");
  }
  CHECK(g_dtors == 6);

  CHECK(rethrow_from_catch() == 7);
  CHECK(throw_inside_catch() == "second");
  CHECK(catch_all() == 1);
  CHECK(unique_ptr_cleanup() == 2);

  // 2. Exceptions on another thread.
  void* h = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
  CHECK(h != nullptr);
  CHECK(WaitForSingleObject(h, 10000) == 0);
  unsigned long code = 0;
  CHECK(GetExitCodeThread(h, &code) && code == 5);
  CloseHandle(h);

  // 3. A few thousand throws to catch leaks or state corruption in the unwinder.
  int caught = 0;
  for (int i = 0; i < 2000; ++i) {
    try {
      if (i % 2) throw i;
      throw std::to_string(i);
    } catch (int) {
      ++caught;
    } catch (const std::string&) {
      ++caught;
    }
  }
  CHECK(caught == 2000);

  OutputDebugStringA(g_failures ? "FAIL cpp_exceptions" : "PASS cpp_exceptions");
  return g_failures ? 1u : 0u;
}
