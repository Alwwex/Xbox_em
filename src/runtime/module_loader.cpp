#include "runtime/module_loader.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <format>
#include <stdexcept>

#include "common/file.h"
#include "common/log.h"
#include "common/strings.h"
#include "platform/memory.h"

namespace xbem::rt {

namespace fs = std::filesystem;
using platform::Prot;

ModuleLoader::ModuleLoader(std::vector<fs::path> search_dirs, TrampolinePool& trampolines)
    : search_dirs_(std::move(search_dirs)), trampolines_(trampolines) {}

LoadedModule& ModuleLoader::load_main(const fs::path& exe) {
  std::lock_guard lock(mutex_);
  LoadedModule* m = load_pe(exe, to_lower(exe.filename().string()));
  if (m->is_dll) XBEM_WARN("loader", "{} is a DLL, not an executable", m->name);
  main_ = m;
  return *m;
}

std::optional<fs::path> ModuleLoader::locate(std::string_view file_name) const {
  for (const fs::path& dir : search_dirs_)
    if (auto p = find_icase(dir, file_name)) return p;
  return std::nullopt;
}

LoadedModule* ModuleLoader::find_loaded(std::string_view name) {
  std::lock_guard lock(mutex_);
  const std::string key = hle::Registry::normalize_dll_name(name);
  auto it = by_name_.find(key);
  return it == by_name_.end() ? nullptr : it->second;
}

LoadedModule* ModuleLoader::load_library(std::string_view name) {
  std::lock_guard lock(mutex_);
  const std::string key = hle::Registry::normalize_dll_name(name);
  if (auto it = by_name_.find(key); it != by_name_.end()) return it->second;

  if (const hle::ModuleDef* def = hle::Registry::instance().find_module(key)) {
    LoadedModule* m = load_hle(*def);
    by_name_[key] = m;
    return m;
  }
  if (auto path = locate(key)) return load_pe(*path, key);
  return nullptr;
}

LoadedModule* ModuleLoader::load_library_path(const fs::path& host_path) {
  std::lock_guard lock(mutex_);
  const std::string key = to_lower(host_path.filename().string());
  if (auto it = by_name_.find(key); it != by_name_.end()) return it->second;
  std::error_code ec;
  if (!fs::is_regular_file(host_path, ec)) return nullptr;
  return load_pe(host_path, key);
}

LoadedModule* ModuleLoader::load_hle(const hle::ModuleDef& def) {
  // One LoadedModule per HLE module, shared by all of its aliases.
  for (auto& m : modules_)
    if (m->hle == &def) return m.get();

  auto m = std::make_unique<LoadedModule>();
  m->name = def.name;
  m->hle = &def;
  m->is_dll = true;
  m->attached = true;
  // Give the module a real address so HMODULE values are unique and
  // GetModuleHandle/GetProcAddress round-trip. Some code peeks at the "MZ".
  m->size = platform::page_size();
  m->base = static_cast<uint8_t*>(platform::map_anywhere(m->size, platform::kAllocationGranularity, Prot::RW));
  if (!m->base) throw std::runtime_error("out of memory for HLE module " + def.name);
  m->base[0] = 'M';
  m->base[1] = 'Z';
  platform::protect(m->base, m->size, Prot::R);
  LoadedModule* raw = m.get();
  modules_.push_back(std::move(m));
  by_name_[def.name] = raw;
  publish_range(*raw);
  XBEM_DEBUG("loader", "HLE module {} at {:#x}", def.name, raw->handle());
  return raw;
}

LoadedModule* ModuleLoader::load_pe(const fs::path& path, const std::string& name) {
  auto m = std::make_unique<LoadedModule>();
  m->name = name;
  m->host_path = path;
  m->image = std::make_unique<pe::PeImage>(pe::PeImage::load(path));
  for (const std::string& w : m->image->warnings()) XBEM_WARN("loader", "{}: {}", name, w);
  m->is_dll = m->image->is_dll();

  map_image(*m);
  index_exports(*m);

  LoadedModule* raw = m.get();
  modules_.push_back(std::move(m));
  by_name_[name] = raw;  // register before resolving imports: import cycles must find us
  publish_range(*raw);
  XBEM_INFO("loader", "mapped {} at {:#x} (size {:#x}, entry {:#x})", name, raw->handle(), raw->size, raw->entry);

  resolve_imports(*raw);
  setup_tls(*raw);
  apply_protections(*raw);
  pending_init_.push_back(raw);  // after its dependencies (post-order)
  return raw;
}

void ModuleLoader::map_image(LoadedModule& m) {
  const pe::PeImage& img = *m.image;
  const size_t size = platform::align_up(img.size_of_image(), platform::page_size());

  uint8_t* base = nullptr;
  if (img.image_base() && img.image_base() % platform::page_size() == 0)
    base = static_cast<uint8_t*>(platform::map_fixed(img.image_base(), size, Prot::RW));
  if (!base) {
    if (img.relocs_stripped() || img.relocations().empty())
      XBEM_WARN("loader", "{}: preferred base {:#x} unavailable and image has no relocations", m.name,
                img.image_base());
    base = static_cast<uint8_t*>(platform::map_anywhere(size, platform::kAllocationGranularity, Prot::RW));
  }
  if (!base) throw std::runtime_error("cannot allocate address space for " + m.name);
  m.base = base;
  m.size = size;

  const auto& file = img.bytes();
  const size_t hdr = std::min<size_t>({img.optional_header().SizeOfHeaders, file.size(), size});
  std::memcpy(base, file.data(), hdr);

  for (const pe::Section& s : img.sections()) {
    const size_t n = std::min<size_t>(s.raw_size, s.mapped_size());
    if (n == 0) continue;
    if (static_cast<uint64_t>(s.virtual_address) + n > size || static_cast<uint64_t>(s.raw_offset) + n > file.size()) {
      XBEM_WARN("loader", "{}: section {} does not fit, skipped", m.name, s.name);
      continue;
    }
    std::memcpy(base + s.virtual_address, file.data() + s.raw_offset, n);
  }

  const int64_t delta = static_cast<int64_t>(reinterpret_cast<uint64_t>(base) - img.image_base());
  if (delta != 0) {
    size_t applied = 0, unknown = 0;
    for (const pe::Relocation& r : img.relocations()) {
      if (r.type == pe::kRelDir64 && r.rva + 8ull <= size) {
        uint64_t v;
        std::memcpy(&v, base + r.rva, 8);
        v += static_cast<uint64_t>(delta);
        std::memcpy(base + r.rva, &v, 8);
        ++applied;
      } else if (r.type == pe::kRelHighLow && r.rva + 4ull <= size) {
        uint32_t v;
        std::memcpy(&v, base + r.rva, 4);
        v += static_cast<uint32_t>(delta);
        std::memcpy(base + r.rva, &v, 4);
        ++applied;
      } else {
        ++unknown;
      }
    }
    XBEM_INFO("loader", "{}: relocated by {:+#x} ({} fixups{})", m.name, delta, applied,
              unknown ? std::format(", {} unsupported", unknown) : "");
  }
  m.entry = img.entry_rva() ? reinterpret_cast<uint64_t>(base) + img.entry_rva() : 0;
}

void ModuleLoader::index_exports(LoadedModule& m) {
  for (const pe::ExportEntry& e : m.image->exports()) {
    LoadedModule::ExportTarget t;
    if (e.forwarder.empty()) t.address = m.handle() + e.rva;
    else t.forwarder = e.forwarder;
    if (!e.name.empty()) m.exports_by_name[e.name] = t;
    m.exports_by_ordinal[e.ordinal] = std::move(t);
  }
}

void ModuleLoader::resolve_imports(LoadedModule& m) {
  std::vector<TrampolinePool::Request> missing;
  size_t resolved = 0;
  for (const pe::ImportModule& imp : m.image->imports()) {
    if (imp.delay_load) continue;  // resolved lazily by the guest's own delay-load helper
    LoadedModule* dep = load_library(imp.dll_name);
    const std::string dll_display = to_lower(imp.dll_name);
    if (!dep) XBEM_WARN("loader", "{}: dependency {} not found - all its imports become stubs", m.name, imp.dll_name);

    for (const pe::ImportEntry& e : imp.entries) {
      uint64_t addr = 0;
      if (dep) {
        if (dep->hle) {
          if (!e.by_ordinal) {
            const auto look = hle::Registry::instance().find(imp.dll_name, e.name);
            if (look.exp) addr = reinterpret_cast<uint64_t>(look.exp->address);
          }
        } else {
          addr = e.by_ordinal ? get_proc_address(*dep, e.ordinal) : get_proc_address_impl(*dep, e.name, 0);
        }
      }
      auto* slot = reinterpret_cast<uint64_t*>(m.base + e.iat_rva);
      if (e.iat_rva + 8ull > m.size) continue;
      if (addr) {
        *slot = addr;
        ++resolved;
      } else {
        missing.push_back({slot, dll_display, e.display_name()});
      }
    }
  }
  trampolines_.emit(missing);
  XBEM_INFO("loader", "{}: {} imports resolved, {} unimplemented (stubbed)", m.name, resolved, missing.size());
}

void ModuleLoader::setup_tls(LoadedModule& m) {
  if (!m.image->tls()) return;
  const pe::TlsInfo& info = *m.image->tls();
  if (info.directory_rva + sizeof(pe::TlsDirectory64) > m.size) return;
  pe::TlsDirectory64 d;
  std::memcpy(&d, m.base + info.directory_rva, sizeof d);  // read the relocated copy

  if (next_tls_index_ >= kMaxTlsModules) {
    XBEM_ERROR("loader", "{}: too many modules with TLS", m.name);
    return;
  }
  m.tls_index = next_tls_index_++;
  m.tls_template_start = d.StartAddressOfRawData;
  m.tls_template_end = d.EndAddressOfRawData;
  m.tls_zero_fill = d.SizeOfZeroFill;
  const uint32_t align_bits = (d.Characteristics >> 20) & 0xF;
  m.tls_alignment = std::max<uint32_t>(16, align_bits ? (1u << (align_bits - 1)) : 16);

  if (d.AddressOfIndex && m.contains(d.AddressOfIndex)) {
    const auto idx = static_cast<uint32_t>(m.tls_index);
    std::memcpy(reinterpret_cast<void*>(d.AddressOfIndex), &idx, 4);
  }
  if (d.AddressOfCallBacks && m.contains(d.AddressOfCallBacks)) {
    const auto* cb = reinterpret_cast<const uint64_t*>(d.AddressOfCallBacks);
    for (int i = 0; i < 256 && cb[i]; ++i) m.tls_callbacks.push_back(cb[i]);
  }
  XBEM_DEBUG("loader", "{}: TLS index {}, template {} bytes + {} zero, {} callbacks", m.name, m.tls_index,
             m.tls_template_end - m.tls_template_start, m.tls_zero_fill, m.tls_callbacks.size());
}

void ModuleLoader::apply_protections(LoadedModule& m) {
  const pe::PeImage& img = *m.image;
  const size_t ps = platform::page_size();
  if (img.optional_header().SectionAlignment < ps) {
    // Sections share pages; we cannot give them distinct protections.
    platform::protect(m.base, m.size, Prot::RWX);
    return;
  }
  platform::protect(m.base, platform::align_up(img.optional_header().SizeOfHeaders, ps), Prot::R);
  for (const pe::Section& s : img.sections()) {
    const size_t len = platform::align_up(s.mapped_size(), ps);
    if (len == 0 || s.virtual_address + len > m.size) continue;
    Prot p;
    if (s.executable()) p = s.writable() ? Prot::RWX : Prot::RX;
    else p = s.writable() ? Prot::RW : Prot::R;
    if (!platform::protect(m.base + s.virtual_address, len, p))
      XBEM_WARN("loader", "{}: cannot set protection of section {}", m.name, s.name);
  }
}

uint64_t ModuleLoader::resolve_export(LoadedModule& m, const LoadedModule::ExportTarget& t, int depth) {
  if (t.forwarder.empty()) return t.address;
  if (depth > 8) {
    XBEM_WARN("loader", "{}: export forwarder chain too long ({})", m.name, t.forwarder);
    return 0;
  }
  // "OTHERDLL.Function" or "OTHERDLL.#12"
  const size_t dot = t.forwarder.rfind('.');
  if (dot == std::string::npos) return 0;
  LoadedModule* target = load_library(t.forwarder.substr(0, dot) + ".dll");
  if (!target) return 0;
  const std::string fn = t.forwarder.substr(dot + 1);
  if (!fn.empty() && fn[0] == '#') return get_proc_address(*target, static_cast<uint16_t>(std::atoi(fn.c_str() + 1)));
  return get_proc_address_impl(*target, fn, depth + 1);
}

uint64_t ModuleLoader::get_proc_address(LoadedModule& m, std::string_view name) {
  std::lock_guard lock(mutex_);
  return get_proc_address_impl(m, name, 0);
}

uint64_t ModuleLoader::get_proc_address_impl(LoadedModule& m, std::string_view name, int depth) {
  if (m.hle) {
    const auto look = hle::Registry::instance().find(m.hle->name, name);
    return look.exp ? reinterpret_cast<uint64_t>(look.exp->address) : 0;
  }
  auto it = m.exports_by_name.find(std::string(name));
  return it == m.exports_by_name.end() ? 0 : resolve_export(m, it->second, depth);
}

uint64_t ModuleLoader::get_proc_address(LoadedModule& m, uint16_t ordinal) {
  std::lock_guard lock(mutex_);
  if (m.hle) return 0;  // HLE modules export by name only
  auto it = m.exports_by_ordinal.find(ordinal);
  return it == m.exports_by_ordinal.end() ? 0 : resolve_export(m, it->second, 0);
}

LoadedModule* ModuleLoader::find_by_handle(uint64_t handle) {
  std::lock_guard lock(mutex_);
  for (auto& m : modules_)
    if (m->handle() == handle) return m.get();
  return nullptr;
}

LoadedModule* ModuleLoader::find_containing(uint64_t address) {
  std::lock_guard lock(mutex_);
  for (auto& m : modules_)
    if (m->contains(address)) return m.get();
  return nullptr;
}

std::vector<LoadedModule*> ModuleLoader::take_uninitialized() {
  std::lock_guard lock(mutex_);
  std::vector<LoadedModule*> out;
  out.swap(pending_init_);
  return out;
}

std::vector<LoadedModule*> ModuleLoader::initialized_modules() {
  std::lock_guard lock(mutex_);
  std::vector<LoadedModule*> out;
  for (auto& m : modules_)
    if (m->attached && !m->hle) out.push_back(m.get());
  return out;
}

std::vector<LoadedModule*> ModuleLoader::all_modules() {
  std::lock_guard lock(mutex_);
  std::vector<LoadedModule*> out;
  for (auto& m : modules_) out.push_back(m.get());
  return out;
}

void ModuleLoader::publish_range(const LoadedModule& m) {
  const size_t i = range_count_.load();
  if (i >= ranges_.size()) return;
  ranges_[i].base = m.handle();
  ranges_[i].size = m.size;
  std::snprintf(ranges_[i].name, sizeof ranges_[i].name, "%s", m.name.c_str());
  range_count_.store(i + 1, std::memory_order_release);
}

bool ModuleLoader::describe(uint64_t address, char* out, size_t out_size) const {
  const size_t n = range_count_.load(std::memory_order_acquire);
  for (size_t i = 0; i < n; ++i) {
    const Range& r = ranges_[i];
    if (address >= r.base && address - r.base < r.size) {
      std::snprintf(out, out_size, "%s+0x%llx", r.name, static_cast<unsigned long long>(address - r.base));
      return true;
    }
  }
  return false;
}

}  // namespace xbem::rt
