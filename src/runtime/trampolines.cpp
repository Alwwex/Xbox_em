#include "runtime/trampolines.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <initializer_list>
#include <stdexcept>

#include "common/log.h"
#include "hle/guest_types.h"
#include "platform/memory.h"
#include "runtime/process.h"

namespace xbem::rt {

namespace {

std::atomic<bool> g_strict{false};
constexpr size_t kStubSize = 64;
constexpr uint64_t kLogFirstCalls = 3;

std::string describe_code(uint64_t address) {
  char buf[256] = {0};
  if (Process* p = Process::current_or_null(); p && p->loader().describe(address, buf, sizeof buf)) return buf;
  return std::format("{:#x}", address);
}

// Called by every stub with the guest's first four integer arguments and the
// guest return address, so the log shows what the title passed and from where.
uint64_t XBEM_GUEST_ABI unimplemented_called(UnimplementedImport* info, uint64_t a1, uint64_t a2, uint64_t a3,
                                             uint64_t a4, uint64_t return_address) {
  const uint64_t n = info->calls.fetch_add(1) + 1;
  if (g_strict.load()) {
    XBEM_ERROR("hle", "guest called unimplemented import {}!{}({:#x}, {:#x}, {:#x}, {:#x}) from {} (strict mode) - "
               "terminating",
               info->dll, info->function, a1, a2, a3, a4, describe_code(return_address));
    if (Process* p = Process::current_or_null()) p->exit_process(125);
    std::fflush(nullptr);
    std::_Exit(125);
  }
  if (n <= kLogFirstCalls) {
    XBEM_WARN("hle", "unimplemented: {}!{}({:#x}, {:#x}, {:#x}, {:#x}) from {} -> returning 0{}", info->dll,
              info->function, a1, a2, a3, a4, describe_code(return_address),
              n == kLogFirstCalls ? " (further calls not logged)" : "");
  }
  set_guest_last_error(win::ERROR_CALL_NOT_IMPLEMENTED);
  return 0;
}

// x86-64 machine code of a stub. Entry has rsp % 16 == 8 (after the guest's call).
//   sub  rsp, 0x38            ; shadow space + 2 stack args, realigned to 16
//   mov  [rsp+0x20], r9       ; guest arg 4 -> handler arg 5
//   mov  rax, [rsp+0x38]      ; guest return address
//   mov  [rsp+0x28], rax      ;   -> handler arg 6
//   mov  r9, r8               ; shift guest args 1..3 one register to the right
//   mov  r8, rdx
//   mov  rdx, rcx
//   mov  rcx, imm64           ; info pointer (first Microsoft x64 argument)
//   mov  rax, imm64           ; handler address
//   call rax
//   add  rsp, 0x38
//   ret
void write_stub(uint8_t* p, uint64_t info, uint64_t handler) {
  size_t i = 0;
  auto emit = [&](std::initializer_list<uint8_t> bytes) {
    for (uint8_t b : bytes) p[i++] = b;
  };
  emit({0x48, 0x83, 0xEC, 0x38});        // sub rsp, 0x38
  emit({0x4C, 0x89, 0x4C, 0x24, 0x20});  // mov [rsp+0x20], r9
  emit({0x48, 0x8B, 0x44, 0x24, 0x38});  // mov rax, [rsp+0x38]
  emit({0x48, 0x89, 0x44, 0x24, 0x28});  // mov [rsp+0x28], rax
  emit({0x4D, 0x89, 0xC1});              // mov r9, r8
  emit({0x49, 0x89, 0xD0});              // mov r8, rdx
  emit({0x48, 0x89, 0xCA});              // mov rdx, rcx
  emit({0x48, 0xB9});                    // mov rcx, imm64
  std::memcpy(p + i, &info, 8);
  i += 8;
  emit({0x48, 0xB8});  // mov rax, imm64
  std::memcpy(p + i, &handler, 8);
  i += 8;
  emit({0xFF, 0xD0});                    // call rax
  emit({0x48, 0x83, 0xC4, 0x38, 0xC3});  // add rsp, 0x38; ret
  while (i < kStubSize) p[i++] = 0xCC;   // int3 padding
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
  std::unique_lock lock(mutex_, std::try_to_lock);  // also used from signal handlers: never block
  if (!lock.owns_lock()) return nullptr;
  const uint64_t base = address - (address % kStubSize);
  auto it = info_by_stub_.find(base);
  return it == info_by_stub_.end() ? nullptr : it->second;
}

}  // namespace xbem::rt
