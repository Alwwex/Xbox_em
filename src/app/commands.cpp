#include "app/commands.h"

#include <algorithm>
#include <cstdio>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>

#include "common/file.h"
#include "common/log.h"
#include "common/strings.h"
#include "gpu/rdna2.h"
#include "hle/registry.h"
#include "loader/pe_image.h"
#include "package/package_layout.h"
#include "package/xvd.h"
#include "platform/cpu.h"
#include "runtime/process.h"

namespace xbem::app {

namespace fs = std::filesystem;

namespace {

template <class... A>
void out(std::format_string<A...> fmt, A&&... a) {
  const std::string s = std::format(fmt, std::forward<A>(a)...);
  std::fwrite(s.data(), 1, s.size(), stdout);
}

// ------------------------------------------------------------ classification

enum class Status { Implemented, Stub, MissingInHle, GuestDll, GuestMissing, Unknown };

const char* status_name(Status s) {
  switch (s) {
    case Status::Implemented: return "implemented";
    case Status::Stub: return "stub";
    case Status::MissingInHle: return "missing";
    case Status::GuestDll: return "guest";
    case Status::GuestMissing: return "guest-missing";
    case Status::Unknown: return "unknown-dll";
  }
  return "?";
}

char status_mark(Status s) {
  switch (s) {
    case Status::Implemented: return '+';
    case Status::Stub: return '~';
    case Status::GuestDll: return 'g';
    default: return '-';
  }
}

// Exports of the DLLs that ship with the game, keyed by lower-case DLL name.
struct GuestIndex {
  std::map<std::string, std::set<std::string>> exports;

  void add(const std::string& dll_name, const pe::PeImage& img) {
    auto& set = exports[hle::Registry::normalize_dll_name(dll_name)];
    for (const auto& e : img.exports()) {
      if (!e.name.empty()) set.insert(e.name);
      set.insert("#" + std::to_string(e.ordinal));
    }
  }
};

Status classify(const std::string& dll, const pe::ImportEntry& e, const GuestIndex& guest) {
  const auto& reg = hle::Registry::instance();
  if (reg.find_module(dll)) {
    const auto look = e.by_ordinal ? reg.find_ordinal(dll, e.ordinal) : reg.find(dll, e.name);
    if (!look.exp) return Status::MissingInHle;
    return look.exp->kind == hle::ExportKind::Stub ? Status::Stub : Status::Implemented;
  }
  auto it = guest.exports.find(hle::Registry::normalize_dll_name(dll));
  if (it != guest.exports.end()) return it->second.count(e.display_name()) ? Status::GuestDll : Status::GuestMissing;
  return Status::Unknown;
}

std::string dll_kind(const std::string& dll, const GuestIndex& guest) {
  if (hle::Registry::instance().find_module(dll)) return "HLE";
  if (guest.exports.count(hle::Registry::normalize_dll_name(dll))) return "guest DLL";
  return "not found";
}

std::string section_flags(const pe::Section& s) {
  std::string f;
  f += s.readable() ? 'R' : '-';
  f += s.writable() ? 'W' : '-';
  f += s.executable() ? 'X' : '-';
  return f;
}

GuestIndex index_directory(const fs::path& dir, const fs::path& skip) {
  GuestIndex gi;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec) || !iequals(it->path().extension().string(), ".dll")) continue;
    if (fs::equivalent(it->path(), skip, ec)) continue;
    try {
      gi.add(it->path().filename().string(), pe::PeImage::load(it->path()));
    } catch (const std::exception&) {
    }
  }
  return gi;
}

std::string json_escape(std::string_view s) {
  std::string o;
  for (char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) o += std::format("\\u{:04x}", c);
        else o += c;
    }
  }
  return o;
}

}  // namespace

// ------------------------------------------------------------------ inspect

int cmd_inspect(const Args& args) {
  if (args.empty()) throw std::runtime_error("usage: xbem inspect <file.exe|dll> [--missing-only]");
  const fs::path path = args[0];
  const bool missing_only = std::find(args.begin(), args.end(), "--missing-only") != args.end();
  const pe::PeImage img = pe::PeImage::load(path);
  const auto& oh = img.optional_header();

  out("File:         {}\n", path.string());
  out("Type:         {} (AMD64), subsystem {} ({})\n", img.is_dll() ? "DLL" : "EXE",
      pe::PeImage::subsystem_name(oh.Subsystem), oh.Subsystem);
  out("Image base:   {:#x}   size {:#x}   entry RVA {:#x}\n", oh.ImageBase, oh.SizeOfImage, oh.AddressOfEntryPoint);
  out("Stack:        reserve {:#x}  commit {:#x}\n", oh.SizeOfStackReserve, oh.SizeOfStackCommit);
  out("Versions:     linker {}.{}  OS {}.{}  subsystem {}.{}\n", oh.MajorLinkerVersion, oh.MinorLinkerVersion,
      oh.MajorOperatingSystemVersion, oh.MinorOperatingSystemVersion, oh.MajorSubsystemVersion,
      oh.MinorSubsystemVersion);
  std::string dllchars;
  if (oh.DllCharacteristics & pe::kDllCharDynamicBase) dllchars += " DYNAMIC_BASE";
  if (oh.DllCharacteristics & pe::kDllCharHighEntropyVa) dllchars += " HIGH_ENTROPY_VA";
  if (oh.DllCharacteristics & pe::kDllCharNxCompat) dllchars += " NX_COMPAT";
  out("Flags:       {}{}\n", dllchars.empty() ? " none" : dllchars, img.relocs_stripped() ? " RELOCS_STRIPPED" : "");
  if (img.codeview())
    out("PDB:          {} (guid {}, age {})\n", img.codeview()->pdb_path, img.codeview()->guid, img.codeview()->age);

  out("Sections:\n");
  for (const pe::Section& s : img.sections())
    out("  {:<9} va {:#010x}  vsize {:#010x}  raw {:#010x}  {}\n", s.name, s.virtual_address, s.virtual_size,
        s.raw_size, section_flags(s));

  if (img.tls()) {
    const auto& t = *img.tls();
    out("TLS:          template {} bytes + {} zero-fill, callbacks array {}\n", t.end_va - t.start_va, t.zero_fill,
        t.callbacks_va ? "present" : "none");
  }
  out("Exceptions:   {} RUNTIME_FUNCTION entries (.pdata)\n", img.exception_function_count());
  out("Relocations:  {}\n", img.relocations().size());
  out("Exports:      {}{}\n", img.exports().size(), img.export_name().empty() ? "" : " (" + img.export_name() + ")");
  for (const std::string& w : img.warnings()) out("Warning:      {}\n", w);

  const GuestIndex guest = index_directory(path.parent_path().empty() ? fs::path(".") : path.parent_path(), path);
  std::map<Status, size_t> totals;
  size_t functions = 0;
  for (const auto& m : img.imports())
    for (const auto& e : m.entries) {
      ++totals[classify(m.dll_name, e, guest)];
      ++functions;
    }
  out("\nImports:      {} functions from {} DLLs\n", functions, img.imports().size());
  out("  HLE implemented {}, HLE stub {}, missing in HLE {}, provided by game DLLs {}, unresolved {}\n",
      totals[Status::Implemented], totals[Status::Stub], totals[Status::MissingInHle], totals[Status::GuestDll],
      totals[Status::GuestMissing] + totals[Status::Unknown]);
  out("  legend: + implemented  ~ stub (reports 'not supported')  g game DLL  - missing\n");

  for (const auto& m : img.imports()) {
    size_t ok = 0;
    for (const auto& e : m.entries) {
      const Status s = classify(m.dll_name, e, guest);
      ok += s == Status::Implemented || s == Status::GuestDll;
    }
    out("\n  {}{}  [{}]  {}/{} resolved\n", m.dll_name, m.delay_load ? " (delay-load)" : "", dll_kind(m.dll_name, guest),
        ok, m.entries.size());
    for (const auto& e : m.entries) {
      const Status s = classify(m.dll_name, e, guest);
      if (missing_only && (s == Status::Implemented || s == Status::GuestDll)) continue;
      out("    {} {}\n", status_mark(s), e.display_name());
    }
  }
  return 0;
}

// ----------------------------------------------------------------- coverage

int cmd_coverage(const Args& args) {
  if (args.empty()) throw std::runtime_error("usage: xbem coverage <dir> [--json FILE] [--markdown FILE] [--all]");
  std::string json_path, md_path;
  bool show_all = false;
  for (size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "--json" && i + 1 < args.size()) json_path = args[++i];
    else if (args[i] == "--markdown" && i + 1 < args.size()) md_path = args[++i];
    else if (args[i] == "--all") show_all = true;
  }
  const package::PackageLayout layout = package::scan_package(args[0]);

  struct Parsed {
    fs::path rel;
    pe::PeImage image;
  };
  std::vector<Parsed> parsed;
  GuestIndex guest;
  for (const fs::path& rel : layout.pe_files) {
    try {
      Parsed p{rel, pe::PeImage::load(layout.root / rel)};
      if (p.image.is_dll()) guest.add(rel.filename().string(), p.image);
      parsed.push_back(std::move(p));
    } catch (const std::exception& e) {
      out("skipping {}: {}\n", rel.generic_string(), e.what());
    }
  }

  struct FnInfo {
    Status status;
    std::set<std::string> users;
  };
  std::map<std::string, std::map<std::string, FnInfo>> by_dll;  // dll -> function -> info
  for (const Parsed& p : parsed)
    for (const auto& m : p.image.imports())
      for (const auto& e : m.entries) {
        const std::string dll = hle::Registry::normalize_dll_name(m.dll_name);
        auto& fi = by_dll[dll][e.display_name()];
        fi.status = classify(m.dll_name, e, guest);
        fi.users.insert(p.rel.generic_string());
      }

  out("Scanned {} PE files in {}\n\n", parsed.size(), layout.root.string());
  out("{:<44} {:>6} {:>6} {:>6} {:>8}  {}\n", "DLL", "total", "impl", "stub", "missing", "kind");
  struct Missing {
    std::string dll, fn;
    size_t users;
  };
  std::vector<Missing> missing;
  size_t grand_total = 0, grand_impl = 0;
  for (const auto& [dll, fns] : by_dll) {
    size_t impl = 0, stub = 0, miss = 0;
    for (const auto& [fn, fi] : fns) {
      if (fi.status == Status::Implemented || fi.status == Status::GuestDll) ++impl;
      else if (fi.status == Status::Stub) ++stub;
      else {
        ++miss;
        missing.push_back({dll, fn, fi.users.size()});
      }
    }
    grand_total += fns.size();
    grand_impl += impl;
    out("{:<44} {:>6} {:>6} {:>6} {:>8}  {}\n", dll, fns.size(), impl, stub, miss, dll_kind(dll, guest));
  }
  out("\nOverall: {}/{} distinct imports resolved ({:.1f}%)\n", grand_impl, grand_total,
      grand_total ? 100.0 * static_cast<double>(grand_impl) / static_cast<double>(grand_total) : 100.0);

  std::sort(missing.begin(), missing.end(), [](const Missing& a, const Missing& b) {
    return a.users != b.users ? a.users > b.users : (a.dll != b.dll ? a.dll < b.dll : a.fn < b.fn);
  });
  const size_t limit = show_all ? missing.size() : std::min<size_t>(missing.size(), 40);
  if (limit) {
    out("\nMost referenced missing imports{}:\n", show_all ? "" : " (top 40, --all for everything)");
    for (size_t i = 0; i < limit; ++i)
      out("  {:>3} module(s)  {}!{}\n", missing[i].users, missing[i].dll, missing[i].fn);
  }

  if (!json_path.empty()) {
    std::ofstream js(json_path);
    js << "{\n  \"root\": \"" << json_escape(layout.root.generic_string()) << "\",\n  \"pe_files\": " << parsed.size()
       << ",\n  \"dlls\": [\n";
    size_t di = 0;
    for (const auto& [dll, fns] : by_dll) {
      js << "    {\"name\": \"" << json_escape(dll) << "\", \"kind\": \"" << dll_kind(dll, guest)
         << "\", \"functions\": [\n";
      size_t fi_n = 0;
      for (const auto& [fn, fi] : fns) {
        js << "      {\"name\": \"" << json_escape(fn) << "\", \"status\": \"" << status_name(fi.status)
           << "\", \"used_by\": [";
        size_t u = 0;
        for (const auto& user : fi.users) js << (u++ ? ", " : "") << "\"" << json_escape(user) << "\"";
        js << "]}" << (++fi_n < fns.size() ? "," : "") << "\n";
      }
      js << "    ]}" << (++di < by_dll.size() ? "," : "") << "\n";
    }
    js << "  ]\n}\n";
    out("\nJSON report written to {}\n", json_path);
  }
  if (!md_path.empty()) {
    std::ofstream md(md_path);
    md << "# Import coverage: " << layout.root.filename().string() << "\n\n";
    md << "| DLL | kind | total | resolved | stub | missing |\n|---|---|---:|---:|---:|---:|\n";
    for (const auto& [dll, fns] : by_dll) {
      size_t impl = 0, stub = 0, miss = 0;
      for (const auto& [fn, fi] : fns) {
        if (fi.status == Status::Implemented || fi.status == Status::GuestDll) ++impl;
        else if (fi.status == Status::Stub) ++stub;
        else ++miss;
      }
      md << "| " << dll << " | " << dll_kind(dll, guest) << " | " << fns.size() << " | " << impl << " | " << stub
         << " | " << miss << " |\n";
    }
    md << "\n## Missing imports (by number of referencing modules)\n\n";
    for (const Missing& m : missing) md << "- `" << m.dll << "!" << m.fn << "` (" << m.users << ")\n";
    out("Markdown report written to {}\n", md_path);
  }
  return 0;
}

// ------------------------------------------------------------------ package

int cmd_package(const Args& args) {
  if (args.empty()) throw std::runtime_error("usage: xbem package <dir>");
  const package::PackageLayout l = package::scan_package(args[0]);
  out("Root:            {}\n", l.root.string());
  out("Files:           {} ({})\n", l.file_count, format_bytes(l.total_bytes));
  if (l.config_path) out("Config:          {}\n", l.config_path->string());
  else out("Config:          MicrosoftGame.config not found\n");
  if (!l.config_error.empty()) out("Config error:    {}\n", l.config_error);
  if (l.config) {
    const auto& c = *l.config;
    out("Identity:        {} (publisher {}), version {}\n", c.identity_name, c.publisher, c.version);
    out("Display name:    {} / {}\n", c.display_name, c.publisher_display_name);
    out("Title ID:        {}   Store ID: {}   MSA app ID: {}\n", c.title_id.empty() ? "-" : c.title_id,
        c.store_id.empty() ? "-" : c.store_id, c.msa_app_id.empty() ? "-" : c.msa_app_id);
    for (const auto& e : c.executables)
      out("Executable:      {}  (TargetDeviceFamily={}, Id={})\n", e.name, e.target_device_family, e.id);
  }
  if (!l.missing_executable.empty())
    out("Main executable: {}  <-- MISSING: named in MicrosoftGame.config but not in this folder\n",
        l.missing_executable);
  else
    out("Main executable: {}\n", l.main_executable.empty() ? "<not determined>" : l.main_executable.string());
  out("PE files:        {}\n", l.pe_files.size());
  for (const auto& p : l.pe_files) out("  {}\n", p.generic_string());
  if (!l.container_files.empty()) {
    out("Packaged containers (cannot be run directly, see `xbem xvd`):\n");
    for (const auto& p : l.container_files) out("  {}\n", p.generic_string());
  }
  return 0;
}

// ---------------------------------------------------------------------- xvd

int cmd_xvd(const Args& args) {
  if (args.empty()) throw std::runtime_error("usage: xbem xvd <file>");
  const std::vector<uint8_t> head = read_file_prefix(args[0], 0x1000);
  const auto info = package::parse_xvd_header(head);
  if (!info) {
    out("{}: not an XVD/XVC container (no 'msft-xvd' header)\n", args[0]);
    return 1;
  }
  out("File:            {}\n", args[0]);
  out("Format version:  {}\n", info->format_version);
  out("Type:            {}\n", info->type == 0 ? "fixed" : info->type == 1 ? "dynamic" : std::to_string(info->type));
  out("Content type:    {} ({})\n", package::xvd_content_type_name(info->content_type), info->content_type);
  out("Volume flags:    {:#x} [{}]\n", info->volume_flags, package::xvd_flags_to_string(info->volume_flags));
  out("Drive size:      {}\n", format_bytes(info->drive_size));
  out("VDUID:           {}\n", guid_to_string(info->vduid.data()));
  out("UDUID:           {}\n", guid_to_string(info->uduid.data()));
  out("XVC data:        {} bytes{}\n", info->xvc_data_length, info->is_xvc() ? " (XVC = streaming install package)" : "");
  out("Encrypted:       {}\n", info->encrypted() ? "yes" : "no");
  if (info->encrypted())
    out("\nThis container is encrypted. xbem does not decrypt packages. Ask the developer for\n"
        "the loose build folder (the output of the GDK build before makepkg).\n");
  return 0;
}

// ---------------------------------------------------------------------- cpu

int cmd_cpu(const Args&) {
  out("Host CPU: {}\n\n", platform::host_cpu_brand());
  out("Xbox Series X|S uses a custom AMD Zen 2 CPU. Games are executed natively,\n"
      "so the host must support the instruction-set extensions they were built with.\n\n");
  bool all_important = true;
  for (const auto& f : platform::host_cpu_features()) {
    out("  {:<12} {}{}\n", f.name, f.present ? "yes" : "NO", (!f.present && f.important) ? "   <-- likely needed" : "");
    if (f.important && !f.present) all_important = false;
  }
  out("\n{}\n", all_important ? "OK: this CPU supports every extension Zen 2 code is likely to use."
                              : "WARNING: missing extensions - games may crash with SIGILL.");
  return all_important ? 0 : 2;
}

// ---------------------------------------------------------------------- hle

int cmd_hle(const Args& args) {
  const auto& reg = hle::Registry::instance();
  if (args.empty()) {
    for (const auto& m : reg.modules()) {
      size_t stubs = 0;
      for (const auto& e : m.exports) stubs += e.kind == hle::ExportKind::Stub;
      std::string aliases;
      for (const auto& a : m.aliases) aliases += (aliases.empty() ? "" : ", ") + a;
      out("{:<18} {:>4} functions ({} stubs)  {}\n", m.name, m.exports.size(), stubs, m.description);
      if (!aliases.empty()) out("{:<18} aliases: {}\n", "", aliases);
    }
    out("\nAPI-set names (api-ms-win-core-*, ...) are served by kernelx.dll.\n");
    return 0;
  }
  const hle::ModuleDef* m = reg.find_module(args[0]);
  if (!m) throw std::runtime_error("unknown HLE module: " + args[0]);
  std::vector<const hle::ExportDef*> sorted;
  for (const auto& e : m->exports) sorted.push_back(&e);
  std::sort(sorted.begin(), sorted.end(), [](auto* a, auto* b) { return a->name < b->name; });
  for (const auto* e : sorted) out("  {} {}\n", e->kind == hle::ExportKind::Stub ? '~' : '+', e->name);
  return 0;
}

// ---------------------------------------------------------------------- run

// Hex dump of a PE file's bytes at an RVA (for disassembling code or reading
// data the emulator reports addresses of, e.g. "game.exe+0x1234").
int cmd_hexdump(const Args& args) {
  if (args.size() < 2) throw std::runtime_error("usage: xbem hexdump <file.exe|dll> <rva (hex)> [length (default 256)]");
  const pe::PeImage img = pe::PeImage::load(args[0]);
  const uint32_t rva = static_cast<uint32_t>(std::stoul(args[1], nullptr, 16));
  const size_t length = args.size() > 2 ? std::stoul(args[2], nullptr, 0) : 256;
  for (size_t done = 0; done < length; done += 16) {
    const auto off = img.rva_to_offset(rva + static_cast<uint32_t>(done), 1);
    if (!off) {
      std::printf("%08zx: (not backed by file data)\n", rva + done);
      break;
    }
    std::printf("%08zx:", rva + done);
    for (size_t k = 0; k < 16 && done + k < length && *off + k < img.bytes().size(); ++k)
      std::printf(" %02x", img.bytes()[*off + k]);
    std::printf("\n");
  }
  return 0;
}

// Which function (.pdata entry) an RVA belongs to, following chained unwind
// info to the function's real start, plus an objdump command for its code.
// With several RVAs (or "-": read them from stdin, one per line, e.g. objdump
// addresses from a grep) it prints one line per RVA. Addresses inside the
// image (as objdump prints them) are accepted too.
int cmd_func(const Args& args) {
  if (args.size() < 2) throw std::runtime_error("usage: xbem func <file.exe|dll> <rva (hex)>... | -");
  const pe::PeImage img = pe::PeImage::load(args[0]);
  const auto& dir = img.directory(pe::kDirException);
  const uint32_t count = img.exception_function_count();
  if (!dir.VirtualAddress || !count) throw std::runtime_error("the file has no exception directory (.pdata)");
  const uint64_t base = img.image_base();
  const auto entry = [&](uint32_t i) { return img.read_rva<pe::RuntimeFunction>(dir.VirtualAddress + i * 12); };
  // The .pdata entry holding `rva` (indirect entries resolved), if any.
  const auto find = [&](uint32_t rva) -> std::optional<pe::RuntimeFunction> {
    uint32_t lo = 0, hi = count;
    while (lo < hi) {
      const uint32_t mid = lo + (hi - lo) / 2;
      const pe::RuntimeFunction f = entry(mid);
      if (rva < f.BeginAddress) hi = mid;
      else if (rva >= f.EndAddress) lo = mid + 1;
      else return (f.UnwindData & 1) ? img.read_rva<pe::RuntimeFunction>(f.UnwindData & ~1u) : f;
    }
    return std::nullopt;
  };
  // Follows chained unwind info to the primary entry (the function's start).
  const auto primary_of = [&](pe::RuntimeFunction fn) {
    for (int depth = 0; depth < 32; ++depth) {
      const uint32_t head = img.read_rva<uint32_t>(fn.UnwindData);
      if (!(((head & 0xFF) >> 3) & 4)) break;
      const uint8_t codes = static_cast<uint8_t>(head >> 16);
      fn = img.read_rva<pe::RuntimeFunction>(fn.UnwindData + 4 + ((codes + 1u) & ~1u) * 2);
    }
    return fn;
  };
  const auto to_rva = [&](const std::string& text) {
    const uint64_t v = std::stoull(text, nullptr, 16);
    return static_cast<uint32_t>(v >= base && v - base < img.size_of_image() ? v - base : v);
  };

  if (args.size() > 2 || args[1] == "-") {
    std::vector<std::string> list(args.begin() + 1, args.end());
    if (args[1] == "-") {
      list.clear();
      std::string line;
      while (std::getline(std::cin, line)) {
        const size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        const size_t e = line.find_first_of(" \t:", b);
        list.push_back(line.substr(b, e == std::string::npos ? std::string::npos : e - b));
      }
    }
    for (const std::string& text : list) {
      uint32_t rva = 0;
      try {
        rva = to_rva(text);
      } catch (const std::exception&) {
        std::printf("%s: not a hex number\n", text.c_str());
        continue;
      }
      const auto fn = find(rva);
      if (!fn) {
        std::printf("%08x  -\n", rva);
        continue;
      }
      const pe::RuntimeFunction p = primary_of(*fn);
      std::printf("%08x  fn %08x +0x%x\n", rva, p.BeginAddress, rva - p.BeginAddress);
    }
    return 0;
  }

  const uint32_t rva = to_rva(args[1]);
  const auto found = find(rva);
  if (!found) {
    std::printf("%08x: not covered by .pdata (a leaf function without unwind data, or not code)\n", rva);
    return 1;
  }
  const pe::RuntimeFunction fn = *found;
  const uint32_t head = img.read_rva<uint32_t>(fn.UnwindData);
  const uint8_t flags = static_cast<uint8_t>((head & 0xFF) >> 3), codes = static_cast<uint8_t>(head >> 16);
  std::printf("%08x: in .pdata range %08x-%08x, prolog %u bytes, %u unwind codes, frame register %u%s%s\n", rva,
              fn.BeginAddress, fn.EndAddress, (head >> 8) & 0xFF, codes, (head >> 24) & 0x0F,
              flags & 3 ? ", has a handler" : "", flags & 4 ? ", chained" : "");
  const pe::RuntimeFunction primary = primary_of(fn);
  if (primary.BeginAddress != fn.BeginAddress)
    std::printf("function starts at %08x (primary range %08x-%08x)\n", primary.BeginAddress, primary.BeginAddress,
                primary.EndAddress);
  else
    std::printf("function starts at %08x\n", fn.BeginAddress);
  std::printf("offset of %08x in the function: +0x%x\n", rva, rva - primary.BeginAddress);
  const auto objdump = [&](const char* what, const pe::RuntimeFunction& f) {
    std::printf("disassemble %s: objdump -d -M intel --start-address=0x%llx --stop-address=0x%llx %s\n", what,
                static_cast<unsigned long long>(base + f.BeginAddress),
                static_cast<unsigned long long>(base + f.EndAddress), args[0].c_str());
  };
  objdump("the function", primary);
  if (primary.BeginAddress != fn.BeginAddress) objdump("the part holding the RVA", fn);
  return 0;
}

// Disassembles RDNA2 GPU code from a file (e.g. a shader captured with
// run --dump-shaders). Without --offset it lists where programs end
// (s_endpgm) to help locate code inside container formats.
int cmd_gpu_disasm(const Args& args) {
  if (args.empty())
    throw std::runtime_error("usage: xbem gpu-disasm <file> [--offset N] [--size N] [--all]");
  std::ifstream f(args[0], std::ios::binary);
  if (!f) throw std::runtime_error("cannot open " + args[0]);
  const std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  size_t offset = 0, size = data.size();
  bool all = false, have_offset = false;
  for (size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "--offset" && i + 1 < args.size()) {
      offset = std::stoul(args[++i], nullptr, 0);
      have_offset = true;
    } else if (args[i] == "--size" && i + 1 < args.size()) {
      size = std::stoul(args[++i], nullptr, 0);
    } else if (args[i] == "--all") {
      all = true;
    } else {
      throw std::runtime_error("unknown option: " + args[i]);
    }
  }
  if (offset >= data.size()) throw std::runtime_error("offset beyond the end of the file");
  size = std::min(size, data.size() - offset);
  std::printf("%s: %zu bytes", args[0].c_str(), data.size());
  if (data.size() >= 4) std::printf(", starts with %02x %02x %02x %02x", data[0], data[1], data[2], data[3]);
  std::printf("\n");
  if (!have_offset) {
    std::vector<size_t> ends;
    for (size_t p = 0; p + 4 <= data.size(); p += 4)
      if (data[p] == 0x00 && data[p + 1] == 0x00 && data[p + 2] == 0x81 && data[p + 3] == 0xBF) ends.push_back(p);
    std::printf("s_endpgm found at %zu offset(s)%s", ends.size(), ends.empty() ? "\n" : ":");
    for (size_t k = 0; k < ends.size() && k < 16; ++k) std::printf(" 0x%zx", ends[k]);
    if (!ends.empty()) std::printf("\n");
  }
  const gpu::rdna2::Listing l = gpu::rdna2::disassemble_program(data.data() + offset, size, offset, !all);
  std::fputs(l.text.c_str(), stdout);
  std::printf("; %zu instructions, %zu not decoded\n", l.instructions, l.unknown);
  return 0;
}

int cmd_run(const Args& args) {
  if (args.empty()) throw std::runtime_error("usage: xbem run <dir|file.exe> [options] [-- guest args]");
  rt::ProcessOptions opt;
  opt.target = args[0];
  for (size_t i = 1; i < args.size(); ++i) {
    const std::string& a = args[i];
    auto value = [&]() -> const std::string& {
      if (i + 1 >= args.size()) throw std::runtime_error(a + " needs a value");
      return args[++i];
    };
    if (a == "--") {
      opt.guest_args.assign(args.begin() + static_cast<long>(i) + 1, args.end());
      break;
    } else if (a == "--strict") {
      opt.strict = true;
    } else if (a == "--trace-hle") {
      opt.trace_hle = true;
    } else if (a == "--dump-shaders") {
      opt.dump_shaders_dir = value();
    } else if (a == "--continue-after-breakpoint") {
      opt.continue_after_breakpoint = true;
    } else if (a == "--trace-at") {
      std::string list = value();
      for (size_t start = 0; start <= list.size();) {
        const size_t comma = std::min(list.find(',', start), list.size());
        if (comma > start) opt.trace_at.push_back(list.substr(start, comma - start));
        start = comma + 1;
      }
    } else if (a == "--trace-log") {
      opt.trace_log_hits = std::stoi(value());
    } else if (a == "--watch") {
      const std::string& list = value();
      for (size_t start = 0; start <= list.size();) {
        const size_t comma = std::min(list.find(',', start), list.size());
        if (comma > start) opt.watch.push_back(list.substr(start, comma - start));
        start = comma + 1;
      }
    } else if (a == "--watch-log") {
      opt.watch_log_writes = std::stoi(value());
    } else if (a == "--device") {
      const std::string& d = value();
      if (d == "series-x") opt.device = rt::DeviceType::XboxScarlettAnaconda;
      else if (d == "series-s") opt.device = rt::DeviceType::XboxScarlettLockhart;
      else if (d == "devkit") opt.device = rt::DeviceType::XboxScarlettDevkit;
      else throw std::runtime_error("unknown device: " + d);
    } else if (a == "--scratch") {
      opt.scratch_dir = value();
    } else if (a == "--mount") {
      const std::string& m = value();
      if (m.size() < 3 || m[1] != '=') throw std::runtime_error("--mount expects X=DIR");
      opt.extra_mounts.emplace_back(m[0], fs::path(m.substr(2)));
    } else if (a == "--log-level") {
      log::Level lv;
      if (!log::parse_level(value(), lv)) throw std::runtime_error("bad log level");
      log::set_level(lv);
    } else {
      throw std::runtime_error("unknown option: " + a);
    }
  }
  rt::Process process(std::move(opt));
  return process.run();
}

}  // namespace xbem::app
