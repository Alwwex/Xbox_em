#include "runtime/trampolines.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "common/log.h"
#include "hle/guest_types.h"
#include "platform/memory.h"
#include "runtime/process.h"

namespace xbem::rt {

namespace {

std::atomic<bool> g_strict{false};
constexpr size_t kStubSize = 32;
constexpr uint64_t kLogFirstCalls = 3;

uint64_t XBEM_GUEST_ABI unimplemented_called(UnimplementedImport* info) {
  const uint64_t n = info->calls.fetch_add(1) + 1;
  if (g_strict.load()) {
    XBEM_ERROR("hle", "guest called unimplemented import {}!{} (strict mode) - terminating", info->dll,
               info->function);
    if (Process* p = Process::current_or_null()) p->exit_process(125);
    std::fflush(nullptr);
    std::_Exit(125);
  }
  if (n <= kLogFirstCalls) {
    XBEM_WARN("hle", "unimplemented: {}!{} called{} -> returning 0", info->dll, info->function,
              n == kLogFirstCalls ? " (further calls not logged)" : "");
  }
  set_guest_last_error(win::ERROR_CALL_NOT_IMPLEMENTED);
  return 0;
}

// x86-64 machine code of a stub. Entry has rsp % 16 == 8 (after the guest's call).
//   sub  rsp, 0x28            ; 32 bytes shadow space + realign to 16
//   mov  rcx, imm64           ; info pointer (first Microsoft x64 argument)
//   mov  rax, imm64           ; handler address
//   call rax
//   add  rsp, 0x28
//   ret
void write_stub(uint8_t* p, uint64_t info, uint64_t handler) {
  size_t i = 0;
  const uint8_t prologue[] = {0x48, 0x83, 0xEC, 0x28};
  std::memcpy(p + i, prologue, sizeof prologue);
  i += sizeof prologue;
  p[i++] = 0x48;
  p[i++] = 0xB9;
  std::memcpy(p + i, &info, 8);
  i += 8;
  p[i++] = 0x48;
  p[i++] = 0xB8;
  std::memcpy(p + i, &handler, 8);
  i += 8;
  p[i++] = 0xFF;
  p[i++] = 0xD0;
  const uint8_t epilogue[] = {0x48, 0x83, 0xC4, 0x28, 0xC3};
  std::memcpy(p + i, epilogue, sizeof epilogue);
  i += sizeof epilogue;
  while (i < kStubSize) p[i++] = 0xCC;  // int3 padding
}

}  // namespace

void TrampolinePool::set_strict(bool strict) { g_strict.store(strict); }

void TrampolinePool::emit(const std::vector<Request>& requests) {
  if (requests.empty()) return;
  std::lock_guard lock(mutex_);

  // Collect the stubs that do not exist yet.
  std::vector<std::pair<std::string, UnimplementedImport*>> fresh;
  for (const Request& r : requests) {
    const std::string key = r.dll + "!" + r.function;
    if (stub_by_key_.count(key)) continue;
    bool pending = false;
    for (const auto& f : fresh) pending |= f.first == key;
    if (pending) continue;
    UnimplementedImport& info = infos_.emplace_back();
    info.dll = r.dll;
    info.function = r.function;
    fresh.emplace_back(key, &info);
  }

  if (!fresh.empty()) {
    const size_t size = platform::align_up(fresh.size() * kStubSize, platform::page_size());
    auto* code = static_cast<uint8_t*>(platform::map_anywhere(size, platform::page_size(), platform::Prot::RW));
    if (!code) throw std::runtime_error("cannot allocate memory for import stubs");
    for (size_t i = 0; i < fresh.size(); ++i) {
      uint8_t* stub = code + i * kStubSize;
      write_stub(stub, reinterpret_cast<uint64_t>(fresh[i].second), reinterpret_cast<uint64_t>(&unimplemented_called));
      const auto addr = reinterpret_cast<uint64_t>(stub);
      stub_by_key_[fresh[i].first] = addr;
      info_by_stub_[addr] = fresh[i].second;
    }
    if (!platform::protect(code, size, platform::Prot::RX))
      throw std::runtime_error("cannot make import stubs executable");
    regions_.push_back({reinterpret_cast<uint64_t>(code), size});
  }

  for (const Request& r : requests) *r.iat_slot = stub_by_key_.at(r.dll + "!" + r.function);
}

std::vector<const UnimplementedImport*> TrampolinePool::called_imports() const {
  std::lock_guard lock(mutex_);
  std::vector<const UnimplementedImport*> out;
  for (const auto& i : infos_)
    if (i.calls.load() > 0) out.push_back(&i);
  return out;
}

size_t TrampolinePool::stub_count() const {
  std::lock_guard lock(mutex_);
  return infos_.size();
}

bool TrampolinePool::contains(uint64_t address) const {
  for (const Region& r : regions_)
    if (address >= r.base && address < r.base + r.size) return true;
  return false;
}

const UnimplementedImport* TrampolinePool::find_by_stub(uint64_t address) const {
  std::lock_guard lock(mutex_);
  const uint64_t base = address - (address % kStubSize);
  auto it = info_by_stub_.find(base);
  return it == info_by_stub_.end() ? nullptr : it->second;
}

}  // namespace xbem::rt
