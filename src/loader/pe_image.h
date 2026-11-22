#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "loader/pe_format.h"

namespace xbem::pe {

class PeError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct Section {
  std::string name;
  uint32_t virtual_address = 0;
  uint32_t virtual_size = 0;
  uint32_t raw_offset = 0;
  uint32_t raw_size = 0;
  uint32_t characteristics = 0;

  bool readable() const { return characteristics & kScnMemRead; }
  bool writable() const { return characteristics & kScnMemWrite; }
  bool executable() const { return characteristics & kScnMemExecute; }
  uint32_t mapped_size() const { return virtual_size ? virtual_size : raw_size; }
};

struct ImportEntry {
  std::string name;        // empty when imported by ordinal
  uint16_t ordinal = 0;    // valid when by_ordinal
  uint16_t hint = 0;
  bool by_ordinal = false;
  uint32_t iat_rva = 0;    // where the loader writes the resolved address

  std::string display_name() const { return by_ordinal ? "#" + std::to_string(ordinal) : name; }
};

struct ImportModule {
  std::string dll_name;
  bool delay_load = false;
  std::vector<ImportEntry> entries;
};

struct ExportEntry {
  std::string name;        // may be empty (ordinal-only export)
  uint16_t ordinal = 0;    // biased ordinal (Base already added)
  uint32_t rva = 0;        // 0 if forwarder
  std::string forwarder;   // "OTHERDLL.Function" or "OTHERDLL.#12"
};

struct Relocation {
  uint32_t rva;
  uint16_t type;
};

struct TlsInfo {
  uint64_t start_va = 0, end_va = 0, index_va = 0, callbacks_va = 0;
  uint32_t zero_fill = 0;
  uint32_t characteristics = 0;
  uint32_t directory_rva = 0;
};

struct CodeViewInfo {
  std::string pdb_path;
  std::string guid;
  uint32_t age = 0;
};

// A parsed, not yet mapped PE32+ image. All reads are bounds-checked so that
// malformed or truncated files produce PeError instead of crashing.
class PeImage {
 public:
  static PeImage parse(std::vector<uint8_t> file);
  static PeImage load(const std::filesystem::path& path);

  const std::vector<uint8_t>& bytes() const { return file_; }
  const FileHeader& file_header() const { return file_header_; }
  const OptionalHeader64& optional_header() const { return opt_; }
  const std::vector<Section>& sections() const { return sections_; }
  const std::vector<ImportModule>& imports() const { return imports_; }
  const std::vector<ExportEntry>& exports() const { return exports_; }
  const std::vector<Relocation>& relocations() const { return relocations_; }
  const std::optional<TlsInfo>& tls() const { return tls_; }
  const std::optional<CodeViewInfo>& codeview() const { return codeview_; }
  const std::string& export_name() const { return export_name_; }
  const std::vector<std::string>& warnings() const { return warnings_; }

  bool is_dll() const { return file_header_.Characteristics & kFileDll; }
  bool relocs_stripped() const { return file_header_.Characteristics & kFileRelocsStripped; }
  uint64_t image_base() const { return opt_.ImageBase; }
  uint32_t size_of_image() const { return opt_.SizeOfImage; }
  uint32_t entry_rva() const { return opt_.AddressOfEntryPoint; }
  const DataDirectory& directory(int index) const { return opt_.Directories[index]; }
  uint32_t exception_function_count() const { return directory(kDirException).Size / sizeof(RuntimeFunction); }

  const Section* section_for_rva(uint32_t rva) const;
  // File offset of `len` bytes at `rva`, if they are fully backed by file data.
  std::optional<size_t> rva_to_offset(uint32_t rva, size_t len = 1) const;

  template <class T>
  T read_rva(uint32_t rva) const {
    const auto off = rva_to_offset(rva, sizeof(T));
    if (!off) throw PeError("RVA out of range: 0x" + to_hex(rva));
    T v;
    std::memcpy(&v, file_.data() + *off, sizeof(T));
    return v;
  }
  std::string read_string_rva(uint32_t rva, size_t max_len = 4096) const;

  static std::string to_hex(uint64_t v);
  static const char* subsystem_name(uint16_t subsystem);

 private:
  std::vector<uint8_t> file_;
  FileHeader file_header_{};
  OptionalHeader64 opt_{};
  std::vector<Section> sections_;
  std::vector<ImportModule> imports_;
  std::vector<ExportEntry> exports_;
  std::vector<Relocation> relocations_;
  std::optional<TlsInfo> tls_;
  std::optional<CodeViewInfo> codeview_;
  std::string export_name_;
  std::vector<std::string> warnings_;

  template <class T>
  T read_offset(size_t off) const {
    if (off > file_.size() || file_.size() - off < sizeof(T)) throw PeError("file offset out of range");
    T v;
    std::memcpy(&v, file_.data() + off, sizeof(T));
    return v;
  }

  void parse_headers();
  void parse_imports();
  void parse_delay_imports();
  void parse_exports();
  void parse_relocations();
  void parse_tls();
  void parse_debug();
  std::vector<ImportEntry> parse_thunks(uint32_t name_table_rva, uint32_t iat_rva);
};

}  // namespace xbem::pe
