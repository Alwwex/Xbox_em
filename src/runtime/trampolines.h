#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace xbem::rt {

// An import the emulator does not implement. Calls to it land in a generated
// stub that logs the call (first few times), sets ERROR_CALL_NOT_IMPLEMENTED
// and returns 0 - or terminates the process in --strict mode. This lets a game
// get as far as possible while telling you exactly what to implement next.
struct UnimplementedImport {
  std::string dll;
  std::string function;
  std::atomic<uint64_t> calls{0};
};

// An emulated function wrapped for --trace-hle.
struct TracedImport {
  std::string name;
  uint64_t target = 0;
  std::atomic<uint64_t> calls{0};
};

class TrampolinePool {
 public:
  struct Request {
    uint64_t* iat_slot;  // written with the stub address once code is emitted
    std::string dll;
    std::string function;
  };

  // Emits stubs for all requests (deduplicated per dll!function) and patches the IAT slots.
  void emit(const std::vector<Request>& requests);

  std::vector<const UnimplementedImport*> called_imports() const;
  size_t stub_count() const;
  bool contains(uint64_t address) const;
  const UnimplementedImport* find_by_stub(uint64_t address) const;

  static void set_strict(bool strict);

  // --trace-hle: calls to emulated functions go through a thunk that records
  // them (first calls are logged; every thread keeps its last calls for crash
  // reports and thread dumps) and then jumps to the implementation.
  static void set_tracing(bool enabled);
  static bool tracing();
  uint64_t trace_thunk(const std::string& name, uint64_t target);
  // Prints the calling thread's recent emulated-function calls. Signal-safe.
  static void print_call_history(void (*write)(const char*));
  const TracedImport* find_thunk(uint64_t address) const;  // never blocks

 private:
  mutable std::mutex mutex_;
  std::deque<UnimplementedImport> infos_;  // stable addresses
  std::unordered_map<std::string, uint64_t> stub_by_key_;
  std::unordered_map<uint64_t, const UnimplementedImport*> info_by_stub_;
  struct Region {
    uint64_t base;
    size_t size;
  };
  std::vector<Region> regions_;

  std::deque<TracedImport> traced_;
  std::unordered_map<uint64_t, uint64_t> thunk_by_target_;
  std::unordered_map<uint64_t, const TracedImport*> info_by_thunk_;
  uint8_t* thunk_chunk_ = nullptr;
  size_t thunk_used_ = 0;
};

}  // namespace xbem::rt
