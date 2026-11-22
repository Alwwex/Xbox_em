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
};

}  // namespace xbem::rt
