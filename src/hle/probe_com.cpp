#include "hle/probe_com.h"

#include <array>
#include <atomic>
#include <format>
#include <mutex>
#include <utility>

#include "hle/discovery.h"
#include "hle/hle_util.h"
#include "platform/memory.h"

namespace xbem::hle {
namespace {

using namespace win;

constexpr int kSlots = 64;
constexpr int kLoggedCallsPerSlot = 4;

struct ProbeObject {
  void* const* vtable;
  std::string name;
  std::atomic<uint32_t> refs{1};
  std::array<std::atomic<int>, kSlots> calls{};
};

bool in_loaded_image(uint64_t address) {
  char buf[64];
  rt::Process* p = rt::Process::current_or_null();
  return p && p->loader().describe(address, buf, sizeof buf);
}

std::string hex_dump(uint64_t address, size_t length) {
  uint8_t bytes[64];
  length = std::min(length, sizeof bytes);
  if (address < 0x10000 || address >= 0x800000000000ull || !platform::safe_read(address, bytes, length)) return {};
  std::string out;
  for (size_t i = 0; i < length; ++i) out += std::format("{:02x}{}", bytes[i], (i % 8 == 7 && i + 1 < length) ? " " : "");
  return out;
}

uint64_t probe_call(ProbeObject* self, int slot, const uint64_t* args, uint64_t ret);

template <int Slot>
uint64_t XBEM_GUEST_ABI probe_method(ProbeObject* self, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                                     uint64_t a5, uint64_t a6) {
  const uint64_t args[6] = {a1, a2, a3, a4, a5, a6};
  return probe_call(self, Slot, args, XBEM_RETURN_ADDRESS());
}

template <size_t... I>
constexpr std::array<void*, kSlots> make_vtable(std::index_sequence<I...>) {
  return {reinterpret_cast<void*>(&probe_method<static_cast<int>(I)>)...};
}

const std::array<void*, kSlots> g_probe_vtable = make_vtable(std::make_index_sequence<kSlots>{});

uint64_t probe_call(ProbeObject* self, int slot, const uint64_t* args, uint64_t ret) {
  if (slot == 1) return self->refs.fetch_add(1) + 1;  // AddRef
  if (slot == 2) return self->refs.fetch_sub(1) - 1;  // Release (probe objects are never freed)
  const bool log = self->calls[slot].fetch_add(1) < kLoggedCallsPerSlot;

  if (slot == 0) {  // QueryInterface(riid, ppv): the object answers for every interface
    auto** out = reinterpret_cast<void**>(args[1]);
    uint8_t iid[16] = {};
    platform::safe_read(args[0], iid, 16);
    if (log)
      XBEM_WARN("probe", "{}::QueryInterface({{{}}}) from {} -> S_OK (same object)", self->name, guid_to_string(iid),
                describe_code_address(ret));
    if (!out) return static_cast<uint64_t>(static_cast<uint32_t>(E_POINTER));
    self->refs.fetch_add(1);
    *out = self;
    return S_OK;
  }

  // An (interface ID in a loaded image, output pointer) pair: hand out a new probe.
  std::string created;
  // Scanned from the last argument: (riid, ppv) usually ends the list.
  for (int i = 4; i >= 0; --i) {
    if (!in_loaded_image(args[i]) || args[i + 1] < 0x10000 || args[i + 1] >= 0x800000000000ull) continue;
    uint8_t iid[16];
    if (!platform::safe_read(args[i], iid, 16)) continue;
    const std::string child = std::format("{}.m{}", self->name, slot);
    void* object = make_probe_object(child);
    if (!platform::safe_write(args[i + 1], &object, 8)) continue;  // not a writable output pointer
    created = std::format(" -> created {} for {{{}}} (arg {})", child, guid_to_string(iid), i + 1);
    break;
  }

  if (log) {
    std::string text;
    for (int i = 0; i < 6; ++i) {
      text += std::format("{}{:#x}", i ? ", " : "", args[i]);
      if (std::string h = hex_dump(args[i], 48); !h.empty()) text += " [" + h + "]";
      else if (std::string d = describe_arg(args[i]); d.find('"') != std::string::npos) text += " " + d.substr(d.find(" -> ") + 4);
    }
    XBEM_WARN("probe", "{}::method{}({}) from {}{} -> S_OK", self->name, slot, text, describe_code_address(ret),
              created);
  }
  return S_OK;
}

}  // namespace

void* make_probe_object(const std::string& name) {
  auto* o = new ProbeObject;  // lives for the whole process
  o->vtable = g_probe_vtable.data();
  o->name = name;
  return o;
}

}  // namespace xbem::hle
