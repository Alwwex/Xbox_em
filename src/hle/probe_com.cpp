#include "hle/probe_com.h"

#include <array>
#include <atomic>
#include <format>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <utility>

#include "hle/discovery.h"
#include "hle/hle_util.h"
#include "platform/memory.h"

namespace xbem::hle {
namespace {

using namespace win;

constexpr int kSlots = kProbeSlots;
constexpr int kLoggedCallsPerSlot = 4;


// Interface IDs are constants: they live in a read-only, non-executable
// section of an image. A pointer into writable game data followed by a pointer
// is not a (riid, ppv) pair - writing an object there corrupts the title.
bool in_read_only_image_data(uint64_t address) {
  rt::Process* p = rt::Process::current_or_null();
  if (!p) return false;
  rt::LoadedModule* m = p->loader().find_containing(address);
  if (!m || !m->image) return false;
  const uint64_t rva = address - m->handle();
  for (const pe::Section& s : m->image->sections())
    if (rva >= s.virtual_address && rva < s.virtual_address + s.mapped_size())
      return s.readable() && !s.writable() && !s.executable();
  return false;
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

// Built on first use: typed vtables in other files copy it during their own
// static initialisation, whose order relative to this file is unspecified.
const std::array<void*, kSlots>& probe_vtable() {
  static const std::array<void*, kSlots> vtable = make_vtable(std::make_index_sequence<kSlots>{});
  return vtable;
}

// Recent probe calls of this thread, for crash reports.
struct ProbeHistoryEntry {
  const ProbeObject* object;
  int slot;
  uint64_t args[4];
  uint64_t ret;
  char note[48];
};
constexpr uint32_t kProbeHistory = 32;
thread_local ProbeHistoryEntry t_probe_history[kProbeHistory];
thread_local uint32_t t_probe_count = 0;

void remember(const ProbeObject* self, int slot, const uint64_t* args, uint64_t ret, const std::string& note) {
  ProbeHistoryEntry& e = t_probe_history[t_probe_count++ % kProbeHistory];
  e.object = self;
  e.slot = slot;
  std::memcpy(e.args, args, sizeof e.args);
  e.ret = ret;
  std::snprintf(e.note, sizeof e.note, "%s", note.c_str());
}

bool in_current_stack(uint64_t p) {
  rt::GuestThread* t = rt::current_thread();
  if (!t || !t->teb) return false;
  uint64_t lo, hi;
  std::memcpy(&lo, t->teb + rt::teb::kStackLimit, 8);
  std::memcpy(&hi, t->teb + rt::teb::kStackBase, 8);
  return p >= lo && p + 8 <= hi;
}

// An argument pointing into the calling thread's stack at a zeroed 8-byte
// slot: the caller cleared an output variable before the call.
bool zeroed_stack_out_pointer(uint64_t p) {
  rt::GuestThread* t = rt::current_thread();
  if (!t || !t->teb || (p & 7)) return false;
  uint64_t lo, hi;
  std::memcpy(&lo, t->teb + rt::teb::kStackLimit, 8);
  std::memcpy(&hi, t->teb + rt::teb::kStackBase, 8);
  uint64_t value;
  return p >= lo && p + 8 <= hi && platform::safe_read(p, &value, 8) && value == 0;
}

std::atomic<int> g_foreign_calls{0};

uint64_t probe_call(ProbeObject* self, int slot, const uint64_t* args, uint64_t ret) {
  if (!self || self->magic != kProbeMagic) {
    // A probe vtable reached with a `this` that is not the object (e.g. an
    // interface pointer at an offset inside it): log, do not touch it.
    if (g_foreign_calls.fetch_add(1) < 16)
      XBEM_WARN("probe", "method{} called with an unknown this={} (args {:#x}, {:#x}) from {} -> S_OK", slot,
                static_cast<void*>(self), args[0], args[1], describe_code_address(ret));
    remember(nullptr, slot, args, ret, "unknown this");
    return S_OK;
  }
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
    if (!in_read_only_image_data(args[i]) || args[i + 1] < 0x10000 || args[i + 1] >= 0x800000000000ull) continue;
    uint8_t iid[16];
    if (!platform::safe_read(args[i], iid, 16)) continue;
    const std::string child = std::format("{}.m{}", self->name, slot);
    void* object = make_probe_object(child);
    if (!platform::safe_write(args[i + 1], &object, 8)) continue;  // not a writable output pointer
    created = std::format(" -> created {} for {{{}}} (arg {}, stored at {:#x})", child, guid_to_string(iid), i + 1,
                          args[i + 1]);
    break;
  }
  // No interface ID: an output variable the caller zeroed on its stack gets a
  // new object (titles store interfaces this way, then call methods on them).
  // Only the last argument that points into the stack is considered (unused
  // argument registers hold leftovers, input structures come first and may
  // start with zero, so they must not be overwritten).
  if (created.empty()) {
    for (int i = 3; i >= 0; --i) {
      if (!in_current_stack(args[i])) continue;
      if (zeroed_stack_out_pointer(args[i])) {
        const std::string child = std::format("{}.m{}", self->name, slot);
        void* object = make_probe_object(child);
        if (platform::safe_write(args[i], &object, 8))
          created = std::format(" -> created {} into the zeroed output at arg {}", child, i + 1);
      }
      break;
    }
  }
  remember(self, slot, args, ret, created.empty() ? std::string() : created.substr(4));

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

const ProbeVtable& default_probe_vtable() { return probe_vtable(); }

void* make_probe_object(const std::string& name, const ProbeVtable* vtable, void* state) {
  auto* o = new ProbeObject;  // lives for the whole process
  o->vtable = vtable ? vtable->data() : probe_vtable().data();
  o->name = name;
  o->state = state;
  return o;
}

void print_probe_history(void (*write)(const char*)) {
  const uint32_t count = t_probe_count;
  if (!count) return;
  write("Last probe-object calls of this thread (oldest first):\n");
  char line[400], where[200];
  for (uint32_t k = count > kProbeHistory ? count - kProbeHistory : 0; k < count; ++k) {
    const ProbeHistoryEntry& e = t_probe_history[k % kProbeHistory];
    where[0] = 0;
    if (rt::Process* p = rt::Process::current_or_null()) p->loader().describe(e.ret, where, sizeof where);
    std::snprintf(line, sizeof line, "  %s::method%d(%#llx, %#llx, %#llx, %#llx) from %s %s\n",
                  e.object ? e.object->name.c_str() : "?", e.slot, static_cast<unsigned long long>(e.args[0]),
                  static_cast<unsigned long long>(e.args[1]), static_cast<unsigned long long>(e.args[2]),
                  static_cast<unsigned long long>(e.args[3]), where[0] ? where : "?", e.note);
    write(line);
  }
}

void probe_log(ProbeObject* self, int slot, const std::string& text, uint64_t return_address) {
  if (self && self->magic == kProbeMagic) {
    const uint64_t none[4] = {};
    remember(self, slot, none, return_address, text.substr(0, 40));
  }
  if (slot < 0 || slot >= kSlots || self->calls[slot].fetch_add(1) >= kLoggedCallsPerSlot) return;
  XBEM_WARN("probe", "{}::{} from {}", self->name, text, describe_code_address(return_address));
}

}  // namespace xbem::hle
