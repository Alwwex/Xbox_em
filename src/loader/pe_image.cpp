#include "loader/pe_image.h"

#include <algorithm>
#include <cstddef>
#include <format>

#include "common/file.h"
#include "common/strings.h"

namespace xbem::pe {

namespace {
constexpr size_t kMaxImports = 1 << 16;
constexpr size_t kMaxExports = 1 << 20;
}  // namespace

std::string PeImage::to_hex(uint64_t v) { return std::format("{:x}", v); }

const char* PeImage::subsystem_name(uint16_t s) {
  switch (s) {
    case 1: return "Native";
    case 2: return "Windows GUI";
    case 3: return "Windows CUI";
    case 9: return "Windows CE GUI";
    case 10: return "EFI Application";
    case 14: return "Xbox";
    case 16: return "Windows Boot Application";
    default: return "Unknown";
  }
}

PeImage PeImage::load(const std::filesystem::path& path) { return parse(read_file(path)); }

PeImage PeImage::parse(std::vector<uint8_t> file) {
  PeImage img;
  img.file_ = std::move(file);
  img.parse_headers();
  // Optional parts: a damaged directory should not prevent inspecting the rest.
  auto guarded = [&img](const char* what, auto&& fn) {
    try {
      fn();
    } catch (const PeError& e) {
      img.warnings_.push_back(std::format("{}: {}", what, e.what()));
    }
  };
  guarded("imports", [&] { img.parse_imports(); });
  guarded("delay imports", [&] { img.parse_delay_imports(); });
  guarded("exports", [&] { img.parse_exports(); });
  guarded("relocations", [&] { img.parse_relocations(); });
  guarded("tls", [&] { img.parse_tls(); });
  guarded("debug", [&] { img.parse_debug(); });
  return img;
}

void PeImage::parse_headers() {
  const auto dos = read_offset<DosHeader>(0);
  if (dos.e_magic != kDosMagic) throw PeError("not a PE file (missing MZ signature)");
  if (dos.e_lfanew <= 0 || static_cast<size_t>(dos.e_lfanew) >= file_.size()) throw PeError("invalid e_lfanew");
  const size_t nt = static_cast<size_t>(dos.e_lfanew);
  if (read_offset<uint32_t>(nt) != kNtSignature) throw PeError("missing PE signature");
  file_header_ = read_offset<FileHeader>(nt + 4);
  if (file_header_.Machine != kMachineAmd64)
    throw PeError(std::format("unsupported machine type 0x{:04x} (only AMD64 images are supported)",
                              file_header_.Machine));

  const size_t opt_off = nt + 4 + sizeof(FileHeader);
  const uint16_t magic = read_offset<uint16_t>(opt_off);
  if (magic != kOptionalMagic64) throw PeError("not a PE32+ image");
  if (file_header_.SizeOfOptionalHeader < offsetof(OptionalHeader64, Directories))
    throw PeError("optional header too small");

  // The optional header may declare fewer than 16 data directories.
  std::memset(&opt_, 0, sizeof opt_);
  const size_t opt_size = std::min<size_t>(file_header_.SizeOfOptionalHeader, sizeof(OptionalHeader64));
  if (opt_off + opt_size > file_.size()) throw PeError("truncated optional header");
  std::memcpy(&opt_, file_.data() + opt_off, opt_size);
  const uint32_t ndirs = std::min<uint32_t>(opt_.NumberOfRvaAndSizes, 16);
  for (uint32_t i = ndirs; i < 16; ++i) opt_.Directories[i] = {};

  if (opt_.SizeOfImage == 0) throw PeError("SizeOfImage is zero");
  if (opt_.SectionAlignment == 0) throw PeError("SectionAlignment is zero");

  const size_t sec_off = opt_off + file_header_.SizeOfOptionalHeader;
  if (file_header_.NumberOfSections > 96) throw PeError("too many sections");
  for (uint16_t i = 0; i < file_header_.NumberOfSections; ++i) {
    const auto sh = read_offset<SectionHeader>(sec_off + i * sizeof(SectionHeader));
    Section s;
    s.name.assign(sh.Name, strnlen(sh.Name, 8));
    s.virtual_address = sh.VirtualAddress;
    s.virtual_size = sh.VirtualSize;
    s.raw_offset = sh.PointerToRawData;
    s.raw_size = sh.SizeOfRawData;
    s.characteristics = sh.Characteristics;
    if (static_cast<uint64_t>(s.virtual_address) + s.mapped_size() > opt_.SizeOfImage)
      warnings_.push_back(std::format("section {} extends beyond SizeOfImage", s.name));
    if (s.raw_size && static_cast<uint64_t>(s.raw_offset) + s.raw_size > file_.size()) {
      warnings_.push_back(std::format("section {} raw data truncated (file too short)", s.name));
      s.raw_size = s.raw_offset < file_.size() ? static_cast<uint32_t>(file_.size() - s.raw_offset) : 0;
    }
    sections_.push_back(std::move(s));
  }
}

const Section* PeImage::section_for_rva(uint32_t rva) const {
  for (const Section& s : sections_) {
    const uint32_t size = std::max(s.virtual_size, s.raw_size);
    if (rva >= s.virtual_address && rva - s.virtual_address < size) return &s;
  }
  return nullptr;
}

std::optional<size_t> PeImage::rva_to_offset(uint32_t rva, size_t len) const {
  if (rva < opt_.SizeOfHeaders) {
    if (static_cast<uint64_t>(rva) + len <= std::min<uint64_t>(opt_.SizeOfHeaders, file_.size())) return rva;
    return std::nullopt;
  }
  const Section* s = section_for_rva(rva);
  if (!s) return std::nullopt;
  const uint64_t delta = rva - s->virtual_address;
  if (delta + len > s->raw_size) return std::nullopt;  // not backed by file data (e.g. .bss)
  const uint64_t off = static_cast<uint64_t>(s->raw_offset) + delta;
  if (off + len > file_.size()) return std::nullopt;
  return static_cast<size_t>(off);
}

std::string PeImage::read_string_rva(uint32_t rva, size_t max_len) const {
  const auto off = rva_to_offset(rva, 1);
  if (!off) throw PeError("string RVA out of range: 0x" + to_hex(rva));
  const size_t avail = std::min(max_len, file_.size() - *off);
  const char* p = reinterpret_cast<const char*>(file_.data() + *off);
  return std::string(p, strnlen(p, avail));
}

std::vector<ImportEntry> PeImage::parse_thunks(uint32_t name_table_rva, uint32_t iat_rva) {
  std::vector<ImportEntry> out;
  const uint32_t table = name_table_rva ? name_table_rva : iat_rva;
  for (size_t i = 0; i < kMaxImports; ++i) {
    const uint64_t thunk = read_rva<uint64_t>(static_cast<uint32_t>(table + i * 8));
    if (thunk == 0) break;
    ImportEntry e;
    e.iat_rva = static_cast<uint32_t>(iat_rva + i * 8);
    if (thunk & (1ull << 63)) {
      e.by_ordinal = true;
      e.ordinal = static_cast<uint16_t>(thunk & 0xFFFF);
    } else {
      const auto hint_rva = static_cast<uint32_t>(thunk & 0x7FFFFFFF);
      e.hint = read_rva<uint16_t>(hint_rva);
      e.name = read_string_rva(hint_rva + 2, 1024);
    }
    out.push_back(std::move(e));
  }
  return out;
}

void PeImage::parse_imports() {
  const DataDirectory& dir = directory(kDirImport);
  if (!dir.VirtualAddress || !dir.Size) return;
  for (size_t i = 0; i < 4096; ++i) {
    const auto desc = read_rva<ImportDescriptor>(static_cast<uint32_t>(dir.VirtualAddress + i * sizeof(ImportDescriptor)));
    if (desc.Name == 0 && desc.FirstThunk == 0) break;
    ImportModule mod;
    mod.dll_name = read_string_rva(desc.Name, 512);
    mod.entries = parse_thunks(desc.OriginalFirstThunk, desc.FirstThunk);
    imports_.push_back(std::move(mod));
  }
}

void PeImage::parse_delay_imports() {
  const DataDirectory& dir = directory(kDirDelayImport);
  if (!dir.VirtualAddress || !dir.Size) return;
  for (size_t i = 0; i < 4096; ++i) {
    const auto desc =
        read_rva<DelayLoadDescriptor>(static_cast<uint32_t>(dir.VirtualAddress + i * sizeof(DelayLoadDescriptor)));
    if (desc.DllNameRVA == 0) break;
    // Attributes bit 0 set = RVA-based descriptor (all modern linkers). Old VA-based ones are rare on x64.
    if (!(desc.Attributes & 1)) {
      warnings_.push_back("VA-based delay-load descriptor skipped");
      continue;
    }
    ImportModule mod;
    mod.delay_load = true;
    mod.dll_name = read_string_rva(desc.DllNameRVA, 512);
    mod.entries = parse_thunks(desc.ImportNameTableRVA, desc.ImportAddressTableRVA);
    imports_.push_back(std::move(mod));
  }
}

void PeImage::parse_exports() {
  const DataDirectory& dir = directory(kDirExport);
  if (!dir.VirtualAddress || !dir.Size) return;
  const auto ed = read_rva<ExportDirectory>(dir.VirtualAddress);
  if (ed.Name) export_name_ = read_string_rva(ed.Name, 512);
  if (ed.NumberOfFunctions > kMaxExports || ed.NumberOfNames > kMaxExports) throw PeError("export table too large");

  std::vector<std::string> names_by_index(ed.NumberOfFunctions);
  for (uint32_t i = 0; i < ed.NumberOfNames; ++i) {
    const uint32_t name_rva = read_rva<uint32_t>(ed.AddressOfNames + i * 4);
    const uint16_t index = read_rva<uint16_t>(ed.AddressOfNameOrdinals + i * 2);
    if (index < names_by_index.size()) names_by_index[index] = read_string_rva(name_rva, 1024);
  }
  for (uint32_t i = 0; i < ed.NumberOfFunctions; ++i) {
    const uint32_t rva = read_rva<uint32_t>(ed.AddressOfFunctions + i * 4);
    if (rva == 0) continue;
    ExportEntry e;
    e.name = names_by_index[i];
    e.ordinal = static_cast<uint16_t>(ed.Base + i);
    if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size) {
      e.forwarder = read_string_rva(rva, 512);
    } else {
      e.rva = rva;
    }
    exports_.push_back(std::move(e));
  }
}

void PeImage::parse_relocations() {
  const DataDirectory& dir = directory(kDirBaseReloc);
  if (!dir.VirtualAddress || !dir.Size) return;
  uint32_t pos = 0;
  while (pos + sizeof(BaseRelocationBlock) <= dir.Size) {
    const auto block = read_rva<BaseRelocationBlock>(dir.VirtualAddress + pos);
    if (block.SizeOfBlock < sizeof(BaseRelocationBlock) || block.SizeOfBlock > dir.Size - pos) {
      if (block.SizeOfBlock != 0) warnings_.push_back("malformed relocation block");
      break;
    }
    const uint32_t count = (block.SizeOfBlock - sizeof(BaseRelocationBlock)) / 2;
    for (uint32_t i = 0; i < count; ++i) {
      const uint16_t e = read_rva<uint16_t>(dir.VirtualAddress + pos + sizeof(BaseRelocationBlock) + i * 2);
      const uint16_t type = e >> 12;
      if (type == kRelAbsolute) continue;
      relocations_.push_back({block.VirtualAddress + (e & 0x0FFFu), type});
    }
    pos += block.SizeOfBlock;
  }
}

void PeImage::parse_tls() {
  const DataDirectory& dir = directory(kDirTls);
  if (!dir.VirtualAddress || !dir.Size) return;
  const auto t = read_rva<TlsDirectory64>(dir.VirtualAddress);
  TlsInfo info;
  info.start_va = t.StartAddressOfRawData;
  info.end_va = t.EndAddressOfRawData;
  info.index_va = t.AddressOfIndex;
  info.callbacks_va = t.AddressOfCallBacks;
  info.zero_fill = t.SizeOfZeroFill;
  info.characteristics = t.Characteristics;
  info.directory_rva = dir.VirtualAddress;
  if (info.end_va < info.start_va) throw PeError("TLS template end before start");
  tls_ = info;
}

void PeImage::parse_debug() {
  const DataDirectory& dir = directory(kDirDebug);
  if (!dir.VirtualAddress || !dir.Size) return;
  const uint32_t count = dir.Size / sizeof(DebugDirectory);
  for (uint32_t i = 0; i < count && i < 64; ++i) {
    const auto d = read_rva<DebugDirectory>(dir.VirtualAddress + i * sizeof(DebugDirectory));
    if (d.Type != kDebugTypeCodeView || d.SizeOfData < 24) continue;
    const size_t off = d.PointerToRawData;
    if (off + d.SizeOfData > file_.size()) continue;
    if (read_offset<uint32_t>(off) != 0x53445352) continue;  // "RSDS"
    CodeViewInfo cv;
    cv.guid = guid_to_string(file_.data() + off + 4);
    cv.age = read_offset<uint32_t>(off + 20);
    const char* path = reinterpret_cast<const char*>(file_.data() + off + 24);
    cv.pdb_path.assign(path, strnlen(path, d.SizeOfData - 24));
    codeview_ = std::move(cv);
    break;
  }
}

}  // namespace xbem::pe
