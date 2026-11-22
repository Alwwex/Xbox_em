#include <cstdlib>
#include <cstring>
#include <filesystem>

#include "loader/pe_image.h"
#include "test.h"

using namespace xbem;

namespace {

// Builds a minimal but valid PE32+ image in memory: one .text section, an
// import of kernelx.dll!GetTickCount64 and a single DIR64 relocation.
std::vector<uint8_t> build_minimal_pe(uint16_t machine = pe::kMachineAmd64) {
  std::vector<uint8_t> f(0x600, 0);
  auto put16 = [&](size_t o, uint16_t v) { std::memcpy(&f[o], &v, 2); };
  auto put32 = [&](size_t o, uint32_t v) { std::memcpy(&f[o], &v, 4); };
  auto put64 = [&](size_t o, uint64_t v) { std::memcpy(&f[o], &v, 8); };

  put16(0, pe::kDosMagic);
  put32(0x3C, 0x80);
  put32(0x80, pe::kNtSignature);
  // File header @0x84
  put16(0x84, machine);
  put16(0x86, 2);                          // sections
  put16(0x84 + 16, sizeof(pe::OptionalHeader64));
  put16(0x84 + 18, 0x0022);                // executable, large address aware
  // Optional header @0x98
  const size_t oh = 0x98;
  put16(oh, pe::kOptionalMagic64);
  put32(oh + 16, 0x1000);                  // entry
  put64(oh + 24, 0x140000000ull);          // image base
  put32(oh + 32, 0x1000);                  // section alignment
  put32(oh + 36, 0x200);                   // file alignment
  put32(oh + 56, 0x3000);                  // size of image
  put32(oh + 60, 0x200);                   // size of headers
  put16(oh + 68, 3);                       // subsystem CUI
  put64(oh + 72, 0x100000);                // stack reserve
  put32(oh + 108, 16);                     // number of data directories
  const size_t dirs = oh + 112;
  put32(dirs + 8 * pe::kDirImport, 0x2000);
  put32(dirs + 8 * pe::kDirImport + 4, 40);
  put32(dirs + 8 * pe::kDirBaseReloc, 0x2100);
  put32(dirs + 8 * pe::kDirBaseReloc + 4, 12);
  // Section table @0x188
  const size_t st = oh + sizeof(pe::OptionalHeader64);
  std::memcpy(&f[st], ".text", 5);
  put32(st + 8, 0x100);
  put32(st + 12, 0x1000);
  put32(st + 16, 0x200);
  put32(st + 20, 0x200);
  put32(st + 36, pe::kScnCode | pe::kScnMemExecute | pe::kScnMemRead);
  std::memcpy(&f[st + 40], ".rdata", 6);
  put32(st + 48, 0x200);
  put32(st + 52, 0x2000);
  put32(st + 56, 0x200);
  put32(st + 60, 0x400);
  put32(st + 76, pe::kScnInitializedData | pe::kScnMemRead);
  // .text: ret
  f[0x200] = 0xC3;
  // .rdata @ file 0x400 = RVA 0x2000: import descriptor, then thunks and names.
  const size_t rd = 0x400;
  put32(rd + 0, 0x2040);   // OriginalFirstThunk
  put32(rd + 12, 0x2060);  // Name
  put32(rd + 16, 0x2050);  // FirstThunk (IAT)
  put64(rd + 0x40, 0x2070);  // INT[0] -> hint/name
  put64(rd + 0x50, 0x2070);  // IAT[0]
  std::memcpy(&f[rd + 0x60], "kernelx.dll", 11);
  put16(rd + 0x70, 7);
  std::memcpy(&f[rd + 0x72], "GetTickCount64", 14);
  // Base relocation block @ RVA 0x2100: page 0x1000, one DIR64 at +0x80, one padding entry.
  put32(rd + 0x100, 0x1000);
  put32(rd + 0x104, 12);
  put16(rd + 0x108, static_cast<uint16_t>((pe::kRelDir64 << 12) | 0x80));
  put16(rd + 0x10A, 0);
  return f;
}

}  // namespace

TEST(pe_parse_minimal_image) {
  const pe::PeImage img = pe::PeImage::parse(build_minimal_pe());
  CHECK(!img.is_dll());
  CHECK_EQ(img.image_base(), 0x140000000ull);
  CHECK_EQ(img.entry_rva(), 0x1000u);
  CHECK_EQ(img.sections().size(), 2u);
  CHECK_EQ(img.sections()[0].name, std::string(".text"));
  CHECK(img.sections()[0].executable());
  CHECK_EQ(img.imports().size(), 1u);
  CHECK_EQ(img.imports()[0].dll_name, std::string("kernelx.dll"));
  CHECK_EQ(img.imports()[0].entries.size(), 1u);
  CHECK_EQ(img.imports()[0].entries[0].name, std::string("GetTickCount64"));
  CHECK_EQ(img.imports()[0].entries[0].hint, 7);
  CHECK_EQ(img.imports()[0].entries[0].iat_rva, 0x2050u);
  CHECK_EQ(img.relocations().size(), 1u);  // the ABSOLUTE padding entry is skipped
  CHECK_EQ(img.relocations()[0].rva, 0x1080u);
  CHECK(img.warnings().empty());
  CHECK(img.rva_to_offset(0x2060).value_or(0) == 0x460);
  CHECK(!img.rva_to_offset(0x5000).has_value());
}

TEST(pe_rejects_malformed_input) {
  CHECK_THROWS(pe::PeImage::parse({}));
  CHECK_THROWS(pe::PeImage::parse(std::vector<uint8_t>{'M', 'Z'}));
  auto f = build_minimal_pe();
  f[0x80] = 'X';  // break the PE signature
  CHECK_THROWS(pe::PeImage::parse(f));
  CHECK_THROWS(pe::PeImage::parse(build_minimal_pe(0x014C)));  // i386
  auto truncated = build_minimal_pe();
  truncated.resize(0x190);
  CHECK_THROWS(pe::PeImage::parse(truncated));
}

TEST(pe_damaged_import_table_is_a_warning) {
  auto f = build_minimal_pe();
  const uint32_t bad = 0x7FFFFFF0;
  std::memcpy(&f[0x400 + 12], &bad, 4);  // DLL name RVA out of range
  const pe::PeImage img = pe::PeImage::parse(f);
  CHECK(img.imports().empty());
  CHECK(!img.warnings().empty());
}

TEST(pe_real_guest_binary) {
  const char* dir = std::getenv("XBEM_GUEST_DIR");
  if (!dir) return;  // set by CTest when the MinGW guest programs were built
  const auto path = std::filesystem::path(dir) / "tls.exe";
  const pe::PeImage img = pe::PeImage::load(path);
  CHECK(img.tls().has_value());
  bool found = false;
  for (const auto& m : img.imports())
    for (const auto& e : m.entries) found |= m.dll_name == "kernelx.dll" && e.name == "CreateThread";
  CHECK(found);
  CHECK(img.exception_function_count() > 0);
}
