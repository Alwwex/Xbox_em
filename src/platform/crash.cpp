#include "platform/crash.h"

#if defined(_WIN32)

namespace xbem::platform {
void install_crash_handler(AddressDescriber) {}
void setup_thread_signal_stack() {}
void install_thread_dump(AddressDescriber, CallSiteDescriber) {}
void register_dump_thread(uint32_t, uint64_t, uint64_t) {}
void set_memory_describer(AddressDescriber) {}
void set_continue_after_breakpoint(bool) {}
const char* patch_cache_hint_instruction(uint8_t*) { return nullptr; }
void set_thread_context_printer(ThreadContextPrinter) {}
void unregister_dump_thread() {}
}  // namespace xbem::platform

#else

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <initializer_list>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "platform/memory.h"

namespace xbem::platform {

namespace {

AddressDescriber g_describer = nullptr;

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
    case SIGTRAP: return "SIGTRAP (int3 breakpoint: the title hit __debugbreak(), usually a failed check)";
    default: return "signal";
  }
}

// ------------------------------------------------------------ thread dump

struct DumpSlot {
  std::atomic<int> tid{0};
  uint32_t guest_id = 0;
  uint64_t stack_low = 0;
  uint64_t stack_high = 0;
};
constexpr int kMaxDumpThreads = 1024;
DumpSlot g_dump_slots[kMaxDumpThreads];
CallSiteDescriber g_call_site = nullptr;
ThreadContextPrinter g_context_printer = nullptr;
std::atomic<int> g_dump_active{0};
std::atomic<int> g_dump_done{0};
std::atomic<bool> g_print_lock{false};

int dump_signal() { return SIGRTMIN + 4; }
int current_tid() { return static_cast<int>(syscall(SYS_gettid)); }

void lock_print() {
  while (g_print_lock.exchange(true, std::memory_order_acquire)) sched_yield();
}
void unlock_print() { g_print_lock.store(false, std::memory_order_release); }

// True if the bytes before `ret` end with a call instruction.
bool preceded_by_call(uint64_t ret) {
  uint8_t b[7];
  // The 7 bytes can start before a mapped image (a "return address" equal to
  // the image base): read them without faulting.
  if (ret < 0x10007 || !safe_read(ret - 7, b, 7)) return false;
  if (b[2] == 0xE8) return true;                                       // call rel32
  if (b[1] == 0xFF && (b[2] & 0x38) == 0x10 && (b[2] & 0xC7) == 0x05) return true;  // call [rip+disp32]
  if (b[5] == 0xFF && (b[6] & 0xF8) == 0xD0) return true;              // call reg
  if (b[4] == 0xFF && (b[5] & 0xF8) == 0x50) return true;              // call [reg+disp8]
  if (b[1] == 0xFF && (b[2] & 0xF8) == 0x90) return true;              // call [reg+disp32]
  if (b[5] == 0xFF && (b[6] & 0xF8) == 0x10) return true;              // call [reg]
  if (b[3] == 0xFF && b[4] == 0x54) return true;                       // call [reg+reg*s+disp8]
  if (b[4] == 0xFF && b[5] == 0x14) return true;                       // call [reg+reg*s]
  return false;
}

void dump_one(const DumpSlot& slot, void* ctx_raw) {
  char line[512];
  char where[256];
  char call[256];
#if defined(__x86_64__)
  auto* uc = static_cast<ucontext_t*>(ctx_raw);
  const greg_t* g = uc->uc_mcontext.gregs;
  const uint64_t rip = static_cast<uint64_t>(g[REG_RIP]);
  const uint64_t rsp = static_cast<uint64_t>(g[REG_RSP]);
  lock_print();
  where[0] = 0;
  if (g_describer) g_describer(rip, where, sizeof where);
  std::snprintf(line, sizeof line, "--- guest thread %u (host tid %d): RIP %016llx %s\n", slot.guest_id,
                slot.tid.load(), static_cast<unsigned long long>(rip),
                where[0] ? where : "(in xbem / host code: blocked in an emulated API)");
  write_str(line);
  // Scan the stack for return addresses into guest modules.
  const uint64_t high = slot.stack_high ? slot.stack_high : rsp + 0x10000;
  int shown = 0;
  for (uint64_t p = rsp & ~7ull; p + 8 <= high && shown < 24 && p - rsp < (4u << 20); p += 8) {
    const uint64_t v = *reinterpret_cast<const uint64_t*>(p);
    if (v < 0x10000) continue;
    where[0] = 0;
    if (g_describer) g_describer(v, where, sizeof where);
    if (!where[0] || !std::strstr(where, "+0x") || !preceded_by_call(v)) continue;
    call[0] = 0;
    if (g_call_site) g_call_site(v, call, sizeof call);
    std::snprintf(line, sizeof line, "    %s%s%s\n", where, call[0] ? "  " : "", call);
    write_str(line);
    ++shown;
  }
  if (!shown) write_str("    (no guest return addresses found on the stack)\n");
  if (g_context_printer) g_context_printer(&write_str);
  unlock_print();
#else
  (void)slot;
  (void)ctx_raw;
  (void)line;
  (void)where;
  (void)call;
#endif
}

void dump_signal_handler(int, siginfo_t*, void* ctx) {
  const int self = current_tid();
  for (const DumpSlot& s : g_dump_slots)
    if (s.tid.load() == self) {
      dump_one(s, ctx);
      break;
    }
  g_dump_done.fetch_add(1);
}

void interrupt_handler(int sig, siginfo_t*, void* ctx) {
  if (g_dump_active.exchange(1)) {
    if (sig == SIGINT) _exit(130);  // second Ctrl+C while dumping: leave immediately
    return;
  }
  write_str(sig == SIGINT ? "\n================ xbem: interrupted (Ctrl+C) - guest thread dump ================\n"
                          : "\n================ xbem: guest thread dump (Ctrl+\\, continuing) ================\n");
  const int self = current_tid();
  int requested = 0;
  g_dump_done.store(0);
  for (const DumpSlot& s : g_dump_slots) {
    const int tid = s.tid.load();
    if (!tid) continue;
    if (tid == self) {
      dump_one(s, ctx);
    } else if (syscall(SYS_tgkill, getpid(), tid, dump_signal()) == 0) {
      ++requested;
    }
  }
  // Wait (signal-safely) for the other threads to print, at most 3 seconds.
  for (int i = 0; i < 300 && g_dump_done.load() < requested; ++i) {
    timespec ts{0, 10 * 1000 * 1000};
    nanosleep(&ts, nullptr);
  }
  if (g_dump_done.load() < requested)
    write_str("(some threads did not respond - they may be stuck with signals blocked)\n");
  write_str("================================================================================\n");
  if (sig == SIGINT) _exit(130);
  g_dump_active.store(0);
}


AddressDescriber g_memory_describer = nullptr;

// Prints the outermost guest frames (where a deep recursion started).
void print_outermost_frames(const DumpSlot* slot, char* line, size_t line_size, char* where, size_t where_size) {
  if (!slot || !slot->stack_high) return;
  uint64_t found[64];
  int n = 0;
  const uint64_t top = slot->stack_high;
  const uint64_t start = top > 0x40000 ? top - 0x40000 : 0;
  for (uint64_t p = std::max<uint64_t>(start, slot->stack_low) & ~7ull; p + 8 <= top; p += 8) {
    uint64_t v;
    if (!safe_read(p, &v, 8) || v < 0x10000) continue;
    where[0] = 0;
    if (g_describer) g_describer(v, where, where_size);
    if (!where[0] || !std::strstr(where, "+0x")) continue;
    uint8_t before[7];
    if (!safe_read(v - 7, before, 7) || !preceded_by_call(v)) continue;
    found[n % 64] = p;  // address of the stack slot
    ++n;
  }
  if (n == 0) return;
  write_str("Outermost frames (top of the stack, innermost first):\n");
  const int first = n > 12 ? n - 12 : 0;
  for (int k = first; k < n; ++k) {
    const uint64_t p = found[k % 64];
    uint64_t v;
    if (!safe_read(p, &v, 8)) continue;
    where[0] = 0;
    if (g_describer) g_describer(v, where, where_size);
    char call[256] = {0};
    if (g_call_site) g_call_site(v, call, sizeof call);
    std::snprintf(line, line_size, "  [top-0x%05llx] %s%s%s\n", static_cast<unsigned long long>(top - p), where,
                  call[0] ? "  " : "", call);
    write_str(line);
  }
}
std::atomic<int> g_crash_tid{0};

const DumpSlot* slot_of_tid(int tid) {
  for (const DumpSlot& s : g_dump_slots)
    if (s.tid.load() == tid) return &s;
  return nullptr;
}

// Where an address points: module+offset, a guest thread's stack, or a
// VirtualAlloc region (with the allocating call site).
void describe_memory(uint64_t address, char* out, size_t out_size) {
  out[0] = 0;
  if (g_describer) g_describer(address, out, out_size);
  if (out[0]) return;
  for (const DumpSlot& s : g_dump_slots) {
    if (s.tid.load() && address >= s.stack_low && address < s.stack_high) {
      std::snprintf(out, out_size, "stack of guest thread %u [%#llx, %#llx)", s.guest_id,
                    static_cast<unsigned long long>(s.stack_low), static_cast<unsigned long long>(s.stack_high));
      return;
    }
  }
  if (g_memory_describer) g_memory_describer(address, out, out_size);
  if (!out[0]) std::snprintf(out, out_size, "not memory the emulator handed out");
}

std::atomic<bool> g_continue_after_breakpoint{false};

#if defined(__x86_64__)
// Instructions of the console CPU (AMD Zen 2) that the host CPU may lack.
// Cache write-back hints (CLWB, CLFLUSHOPT) are rewritten in place into a NOP
// of the same length (memory is coherent: they have no visible effect);
// CLZERO, MONITORX and MWAITX are emulated each time.
std::atomic<uint32_t> g_patched_kinds{0};

void note_patch(uint32_t kind, const char* what, uint64_t rip) {
  if (g_patched_kinds.fetch_or(kind) & kind) return;
  char line[400], where[256] = {0};
  if (g_describer) g_describer(rip, where, sizeof where);
  std::snprintf(line, sizeof line, "[warn ][cpu  ] %s at %s is not supported by this CPU - %s\n", what,
                where[0] ? where : "?",
                kind <= 2 ? "replaced with a NOP (cache hint only)" : "emulated");
  write_str(line);
}

bool make_writable(uint64_t address, size_t size) {
  const uint64_t page = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
  const uint64_t start = address & ~(page - 1);
  const uint64_t end = (address + size + page - 1) & ~(page - 1);
  return mprotect(reinterpret_cast<void*>(start), end - start, PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
}

struct DecodedPrefixes {
  size_t opcode = 0;  // index of the 0F escape
  bool opsize = false;
};

DecodedPrefixes skip_prefixes(const uint8_t* code) {
  DecodedPrefixes d;
  size_t i = 0;
  for (; i < 4; ++i) {  // legacy prefixes
    const uint8_t b = code[i];
    if (b == 0x66) d.opsize = true;
    else if (!(b == 0xF2 || b == 0xF3 || b == 0x2E || b == 0x3E || b == 0x26 || b == 0x36 || b == 0x64 ||
               b == 0x65 || b == 0x67))
      break;
  }
  if ((code[i] & 0xF0) == 0x40) ++i;  // REX
  d.opcode = i;
  return d;
}

}  // namespace

const char* patch_cache_hint_instruction(uint8_t* code) {
  const DecodedPrefixes d = skip_prefixes(code);
  const size_t i = d.opcode;
  if (code[i] != 0x0F || code[i + 1] != 0xAE || !d.opsize) return nullptr;
  const uint8_t modrm = code[i + 2];
  const int reg = (modrm >> 3) & 7;
  if ((modrm >> 6) == 3 || reg < 6) return nullptr;
  // 66 0F AE /6 CLWB m8, 66 0F AE /7 CLFLUSHOPT m8 -> 66 0F 1F /0 NOP m16: the
  // same length and addressing bytes, and it touches no memory.
  if (!make_writable(reinterpret_cast<uint64_t>(code + i + 1), 2)) return nullptr;
  code[i + 1] = 0x1F;
  code[i + 2] = static_cast<uint8_t>(modrm & ~0x38);
  __builtin___clear_cache(reinterpret_cast<char*>(code), reinterpret_cast<char*>(code + i + 3));
  return reg == 6 ? "CLWB" : "CLFLUSHOPT";
}

namespace {

// Returns true when execution can continue at the (possibly updated) RIP.
bool handle_unsupported_instruction(ucontext_t* uc) {
  greg_t* g = uc->uc_mcontext.gregs;
  auto* code = reinterpret_cast<uint8_t*>(g[REG_RIP]);
  const uint64_t rip = static_cast<uint64_t>(g[REG_RIP]);
  if (const char* what = patch_cache_hint_instruction(code)) {
    note_patch(what[2] == 'W' ? 1 : 2, what, rip);
    return true;
  }
  const DecodedPrefixes d = skip_prefixes(code);
  const size_t i = d.opcode;
  const bool opsize = d.opsize;
  if (code[i] != 0x0F) return false;
  const uint8_t op = code[i + 1], modrm = code[i + 2];
  if (op == 0x1F && opsize && ((modrm >> 3) & 7) == 0) return true;  // patched by another thread meanwhile
  if (op == 0x01 && i == 0) {
    if (modrm == 0xFC) {  // CLZERO: zero the 64-byte line at RAX
      auto* line = reinterpret_cast<uint64_t*>(static_cast<uint64_t>(g[REG_RAX]) & ~63ull);
      for (int k = 0; k < 8; ++k) line[k] = 0;
      g[REG_RIP] += 3;
      note_patch(4, "CLZERO", rip);
      return true;
    }
    if (modrm == 0xFA || modrm == 0xFB) {  // MONITORX / MWAITX: wake up at once
      g[REG_RIP] += 3;
      note_patch(8, modrm == 0xFA ? "MONITORX" : "MWAITX", rip);
      return true;
    }
  }
  return false;
}
#endif

void crash_handler(int sig, siginfo_t* info, void* ctx_raw) {
#if defined(__x86_64__)
  if (sig == SIGILL && handle_unsupported_instruction(static_cast<ucontext_t*>(ctx_raw))) return;
  if (sig == SIGTRAP && g_continue_after_breakpoint.load()) {
    // int3 leaves RIP after the instruction: returning continues the title.
    char line[320], where[256] = {0};
    const uint64_t rip = static_cast<uint64_t>(static_cast<ucontext_t*>(ctx_raw)->uc_mcontext.gregs[REG_RIP]);
    if (g_describer) g_describer(rip - 1, where, sizeof where);
    std::snprintf(line, sizeof line, "[warn ][crash] int3 breakpoint at %s ignored (--continue-after-breakpoint)\n",
                  where[0] ? where : "?");
    write_str(line);
    return;
  }
#endif
  const int self = current_tid();
  int expected = 0;
  if (!g_crash_tid.compare_exchange_strong(expected, self)) {
    if (expected == self) _exit(128 + sig);  // crashed while reporting
    for (;;) pause();                        // another thread is reporting; it will end the process
  }

  char line[600];
  char where[300];
  write_str("\n================ xbem: guest crash ================\n");
  const uint64_t fault = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(info->si_addr));
  const DumpSlot* slot = slot_of_tid(self);
  std::snprintf(line, sizeof line, "%s, fault address 0x%016llx, guest thread %u (host tid %d)\n", signal_name(sig),
                static_cast<unsigned long long>(fault), slot ? slot->guest_id : 0, self);
  write_str(line);

#if defined(__x86_64__)
  auto* uc = static_cast<ucontext_t*>(ctx_raw);
  const greg_t* g = uc->uc_mcontext.gregs;
  const uint64_t rip = static_cast<uint64_t>(g[REG_RIP]);
  const uint64_t rsp = static_cast<uint64_t>(g[REG_RSP]);
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

  // Code around RIP, for disassembly.
  uint8_t code[48];
  if (safe_read(rip - 16, code, sizeof code)) {
    char* o = line;
    o += std::snprintf(o, 40, "Code (RIP-16..RIP+32):");
    for (size_t i = 0; i < sizeof code; ++i)
      o += std::snprintf(o, 8, i == 16 ? " [%02x]" : " %02x", code[i]);
    std::snprintf(o, 4, "\n");
    write_str(line);
  }

  // A jump/call to a bad address (e.g. a NULL function pointer): show the
  // calling instruction, found through the return address at [rsp].
  {
    char probe_where[64] = {0};
    if (g_describer) g_describer(rip, probe_where, sizeof probe_where);
    uint64_t ret = 0;
    uint8_t caller[16];
    if (!probe_where[0] && safe_read(rsp, &ret, 8) && safe_read(ret - 16, caller, 16)) {
      where[0] = 0;
      if (g_describer) g_describer(ret, where, sizeof where);
      char* o = line;
      o += std::snprintf(o, 120, "Bad jump target; return address %s, caller code (ret-16..ret):",
                         where[0] ? where : "?");
      for (int i = 0; i < 16; ++i) o += std::snprintf(o, 8, " %02x", caller[i]);
      std::snprintf(o, 4, "\n");
      write_str(line);
    }
  }

  // Which memory the fault address and the stack pointer belong to.
  describe_memory(fault, where, sizeof where);
  std::snprintf(line, sizeof line, "Fault address: %s\n", where);
  write_str(line);
  describe_memory(rsp, where, sizeof where);
  uint64_t probe;
  std::snprintf(line, sizeof line, "Stack pointer: %s%s\n", where,
                safe_read(rsp, &probe, 8) ? "" : "  ** RSP IS NOT READABLE (stack switched to bad memory?) **");
  write_str(line);
  if (slot && fault < slot->stack_low && fault + 0x100000 >= slot->stack_low)
    write_str("Looks like a STACK OVERFLOW of this guest thread.\n");

  // What the registers point at (code, emulator objects, allocations).
  {
    bool header = false;
    for (const auto& r : regs) {
      if (r.reg == REG_RSP) continue;
      const uint64_t v = static_cast<uint64_t>(g[r.reg]);
      if (v < 0x10000 || v >= 0x800000000000ull) continue;
      where[0] = 0;
      if (g_describer) g_describer(v, where, sizeof where);
      if (!where[0]) {
        describe_memory(v, where, sizeof where);
        if (std::strcmp(where, "not memory the emulator handed out") == 0) continue;
      }
      if (!header) write_str("Registers pointing at known memory:\n");
      header = true;
      std::snprintf(line, sizeof line, "  %s=%016llx  %s\n", r.name, static_cast<unsigned long long>(v), where);
      write_str(line);
    }
  }

  // Return-address scan of the stack (reads are checked, never fault).
  write_str("Stack (likely return addresses):\n");
  int shown = 0;
  // After a stack overflow RSP is below the stack: scan from its bottom instead.
  const uint64_t scan = (!safe_read(rsp, &probe, 8) && slot && rsp < slot->stack_low) ? slot->stack_low : rsp;
  for (uint64_t p = scan & ~7ull; shown < 16 && p - scan < 0x20000; p += 8) {
    uint64_t v;
    if (!safe_read(p, &v, 8)) break;
    if (v < 0x10000) continue;
    where[0] = 0;
    if (g_describer) g_describer(v, where, sizeof where);
    if (!where[0] || !std::strstr(where, "+0x")) continue;
    uint8_t before[7];
    if (!safe_read(v - 7, before, 7) || !preceded_by_call(v)) continue;
    char call[256] = {0};
    if (g_call_site) g_call_site(v, call, sizeof call);
    std::snprintf(line, sizeof line, "  [%s+0x%05llx] %s%s%s\n", scan == rsp ? "rsp" : "stack bottom",
                  static_cast<unsigned long long>(p - scan), where, call[0] ? "  " : "", call);
    write_str(line);
    ++shown;
  }
  if (!shown) write_str("  (none found)\n");
  print_outermost_frames(slot, line, sizeof line, where, sizeof where);
  if (g_context_printer) g_context_printer(&write_str);
#else
  (void)ctx_raw;
  (void)where;
  (void)line;
#endif
  write_str("===================================================\n");
  _exit(128 + sig);
}

}  // namespace

void install_thread_dump(AddressDescriber describer, CallSiteDescriber call_site) {
  if (!g_describer) g_describer = describer;
  g_call_site = call_site;
  struct sigaction sa;
  std::memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = dump_signal_handler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
  sigemptyset(&sa.sa_mask);
  sigaction(dump_signal(), &sa, nullptr);
  sa.sa_sigaction = interrupt_handler;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGQUIT, &sa, nullptr);
}

void set_memory_describer(AddressDescriber describer) { g_memory_describer = describer; }
void set_continue_after_breakpoint(bool enabled) { g_continue_after_breakpoint.store(enabled); }
void set_thread_context_printer(ThreadContextPrinter printer) { g_context_printer = printer; }

void register_dump_thread(uint32_t guest_thread_id, uint64_t stack_low, uint64_t stack_high) {
  const int self = current_tid();
  for (DumpSlot& s : g_dump_slots) {
    int expected = 0;
    if (s.tid.compare_exchange_strong(expected, self)) {
      s.guest_id = guest_thread_id;
      s.stack_low = stack_low;
      s.stack_high = stack_high;
      return;
    }
  }
}

void unregister_dump_thread() {
  const int self = current_tid();
  for (DumpSlot& s : g_dump_slots) {
    int expected = self;
    if (s.tid.compare_exchange_strong(expected, 0)) return;
  }
}

void install_crash_handler(AddressDescriber describer) {
  g_describer = describer;
  struct sigaction sa;
  std::memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = crash_handler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  for (int sig : {SIGSEGV, SIGILL, SIGFPE, SIGBUS, SIGTRAP}) sigaction(sig, &sa, nullptr);
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
