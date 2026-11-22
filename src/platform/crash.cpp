#include "platform/crash.h"

#if defined(_WIN32)

namespace xbem::platform {
void install_crash_handler(AddressDescriber) {}
void setup_thread_signal_stack() {}
}  // namespace xbem::platform

#else

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <signal.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include <atomic>
#include <initializer_list>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace xbem::platform {

namespace {

AddressDescriber g_describer = nullptr;
std::atomic<bool> g_in_handler{false};

void write_str(const char* s) {
  const size_t n = std::strlen(s);
  ssize_t r = ::write(STDERR_FILENO, s, n);
  (void)r;
}

const char* signal_name(int sig) {
  switch (sig) {
    case SIGSEGV: return "SIGSEGV (access violation)";
    case SIGILL: return "SIGILL (illegal instruction - CPU feature missing on host?)";
    case SIGFPE: return "SIGFPE (arithmetic exception)";
    case SIGBUS: return "SIGBUS (bus error)";
    default: return "signal";
  }
}

void crash_handler(int sig, siginfo_t* info, void* ctx_raw) {
  if (g_in_handler.exchange(true)) _exit(128 + sig);  // crashed while reporting

  char line[512];
  char where[256];
  write_str("\n================ xbem: guest crash ================\n");
  std::snprintf(line, sizeof line, "%s, fault address 0x%016llx\n", signal_name(sig),
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(info->si_addr)));
  write_str(line);

#if defined(__x86_64__)
  auto* uc = static_cast<ucontext_t*>(ctx_raw);
  const greg_t* g = uc->uc_mcontext.gregs;
  const uint64_t rip = static_cast<uint64_t>(g[REG_RIP]);
  where[0] = 0;
  if (g_describer) g_describer(rip, where, sizeof where);
  std::snprintf(line, sizeof line, "RIP=%016llx  %s\n", static_cast<unsigned long long>(rip), where);
  write_str(line);

  struct {
    const char* name;
    int reg;
  } regs[] = {{"RAX", REG_RAX}, {"RBX", REG_RBX}, {"RCX", REG_RCX}, {"RDX", REG_RDX}, {"RSI", REG_RSI},
              {"RDI", REG_RDI}, {"RBP", REG_RBP}, {"RSP", REG_RSP}, {"R8 ", REG_R8},  {"R9 ", REG_R9},
              {"R10", REG_R10}, {"R11", REG_R11}, {"R12", REG_R12}, {"R13", REG_R13}, {"R14", REG_R14},
              {"R15", REG_R15}};
  for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); ++i) {
    std::snprintf(line, sizeof line, "%s=%016llx%s", regs[i].name,
                  static_cast<unsigned long long>(g[regs[i].reg]), (i % 4 == 3) ? "\n" : "  ");
    write_str(line);
  }

  // Best-effort return-address scan of the top of the stack.
  write_str("Stack (possible return addresses):\n");
  const uint64_t* sp = reinterpret_cast<const uint64_t*>(g[REG_RSP]);
  int shown = 0;
  for (int i = 0; i < 256 && shown < 12; ++i) {
    const uint64_t v = sp[i];
    where[0] = 0;
    if (g_describer) g_describer(v, where, sizeof where);
    if (where[0] && std::strstr(where, "+0x")) {
      std::snprintf(line, sizeof line, "  [rsp+0x%03x] %016llx  %s\n", i * 8, static_cast<unsigned long long>(v),
                    where);
      write_str(line);
      ++shown;
    }
  }
#else
  (void)ctx_raw;
  (void)where;
#endif
  write_str("===================================================\n");
  _exit(128 + sig);
}

}  // namespace

void install_crash_handler(AddressDescriber describer) {
  g_describer = describer;
  struct sigaction sa;
  std::memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = crash_handler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  for (int sig : {SIGSEGV, SIGILL, SIGFPE, SIGBUS}) sigaction(sig, &sa, nullptr);
}

void setup_thread_signal_stack() {
  constexpr size_t kSize = 64 * 1024;
  stack_t ss;
  ss.ss_sp = std::malloc(kSize);  // intentionally never freed: lives as long as the thread
  ss.ss_size = kSize;
  ss.ss_flags = 0;
  if (ss.ss_sp) sigaltstack(&ss, nullptr);
}

}  // namespace xbem::platform

#endif
