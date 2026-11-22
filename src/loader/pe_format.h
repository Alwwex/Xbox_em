#pragma once

// On-disk structures of the Portable Executable format (PE32+ only).
// Xbox One / Xbox Series executables are ordinary x64 PE images.
// Own names are used so this header can coexist with <windows.h>.

#include <cstdint>

namespace xbem::pe {

#pragma pack(push, 1)

struct DosHeader {
  uint16_t e_magic;  // "MZ"
  uint16_t e_cblp, e_cp, e_crlc, e_cparhdr, e_minalloc, e_maxalloc, e_ss, e_sp, e_csum, e_ip, e_cs, e_lfarlc, e_ovno;
  uint16_t e_res[4];
  uint16_t e_oemid, e_oeminfo;
  uint16_t e_res2[10];
  int32_t e_lfanew;
};
static_assert(sizeof(DosHeader) == 64);

struct FileHeader {
  uint16_t Machine;
  uint16_t NumberOfSections;
  uint32_t TimeDateStamp;
  uint32_t PointerToSymbolTable;
  uint32_t NumberOfSymbols;
  uint16_t SizeOfOptionalHeader;
  uint16_t Characteristics;
};
static_assert(sizeof(FileHeader) == 20);

struct DataDirectory {
  uint32_t VirtualAddress;
  uint32_t Size;
};

struct OptionalHeader64 {
  uint16_t Magic;  // 0x20B
  uint8_t MajorLinkerVersion, MinorLinkerVersion;
  uint32_t SizeOfCode, SizeOfInitializedData, SizeOfUninitializedData;
  uint32_t AddressOfEntryPoint, BaseOfCode;
  uint64_t ImageBase;
  uint32_t SectionAlignment, FileAlignment;
  uint16_t MajorOperatingSystemVersion, MinorOperatingSystemVersion;
  uint16_t MajorImageVersion, MinorImageVersion;
  uint16_t MajorSubsystemVersion, MinorSubsystemVersion;
  uint32_t Win32VersionValue, SizeOfImage, SizeOfHeaders, CheckSum;
  uint16_t Subsystem, DllCharacteristics;
  uint64_t SizeOfStackReserve, SizeOfStackCommit, SizeOfHeapReserve, SizeOfHeapCommit;
  uint32_t LoaderFlags, NumberOfRvaAndSizes;
  DataDirectory Directories[16];
};
static_assert(sizeof(OptionalHeader64) == 240);

struct SectionHeader {
  char Name[8];
  uint32_t VirtualSize, VirtualAddress, SizeOfRawData, PointerToRawData;
  uint32_t PointerToRelocations, PointerToLinenumbers;
  uint16_t NumberOfRelocations, NumberOfLinenumbers;
  uint32_t Characteristics;
};
static_assert(sizeof(SectionHeader) == 40);

struct ImportDescriptor {
  uint32_t OriginalFirstThunk;  // RVA of the import name table
  uint32_t TimeDateStamp;
  uint32_t ForwarderChain;
  uint32_t Name;        // RVA of the DLL name
  uint32_t FirstThunk;  // RVA of the import address table (IAT)
};
static_assert(sizeof(ImportDescriptor) == 20);

struct DelayLoadDescriptor {
  uint32_t Attributes;
  uint32_t DllNameRVA;
  uint32_t ModuleHandleRVA;
  uint32_t ImportAddressTableRVA;
  uint32_t ImportNameTableRVA;
  uint32_t BoundImportAddressTableRVA;
  uint32_t UnloadInformationTableRVA;
  uint32_t TimeDateStamp;
};
static_assert(sizeof(DelayLoadDescriptor) == 32);

struct ExportDirectory {
  uint32_t Characteristics, TimeDateStamp;
  uint16_t MajorVersion, MinorVersion;
  uint32_t Name, Base, NumberOfFunctions, NumberOfNames;
  uint32_t AddressOfFunctions, AddressOfNames, AddressOfNameOrdinals;
};
static_assert(sizeof(ExportDirectory) == 40);

struct BaseRelocationBlock {
  uint32_t VirtualAddress;
  uint32_t SizeOfBlock;
};

struct TlsDirectory64 {
  uint64_t StartAddressOfRawData;  // VAs, relocated like any other pointer
  uint64_t EndAddressOfRawData;
  uint64_t AddressOfIndex;
  uint64_t AddressOfCallBacks;
  uint32_t SizeOfZeroFill;
  uint32_t Characteristics;
};
static_assert(sizeof(TlsDirectory64) == 40);

struct DebugDirectory {
  uint32_t Characteristics, TimeDateStamp;
  uint16_t MajorVersion, MinorVersion;
  uint32_t Type, SizeOfData, AddressOfRawData, PointerToRawData;
};
static_assert(sizeof(DebugDirectory) == 28);

struct RuntimeFunction {
  uint32_t BeginAddress, EndAddress, UnwindData;
};
static_assert(sizeof(RuntimeFunction) == 12);

#pragma pack(pop)

constexpr uint16_t kDosMagic = 0x5A4D;      // "MZ"
constexpr uint32_t kNtSignature = 0x4550;   // "PE\0\0"
constexpr uint16_t kOptionalMagic64 = 0x20B;
constexpr uint16_t kMachineAmd64 = 0x8664;

constexpr uint16_t kFileRelocsStripped = 0x0001;
constexpr uint16_t kFileDll = 0x2000;

enum DirectoryIndex : int {
  kDirExport = 0,
  kDirImport = 1,
  kDirResource = 2,
  kDirException = 3,
  kDirSecurity = 4,
  kDirBaseReloc = 5,
  kDirDebug = 6,
  kDirTls = 9,
  kDirLoadConfig = 10,
  kDirIat = 12,
  kDirDelayImport = 13,
};

constexpr uint32_t kScnCode = 0x00000020;
constexpr uint32_t kScnInitializedData = 0x00000040;
constexpr uint32_t kScnUninitializedData = 0x00000080;
constexpr uint32_t kScnMemExecute = 0x20000000;
constexpr uint32_t kScnMemRead = 0x40000000;
constexpr uint32_t kScnMemWrite = 0x80000000;

constexpr uint16_t kRelAbsolute = 0;
constexpr uint16_t kRelHighLow = 3;
constexpr uint16_t kRelDir64 = 10;

constexpr uint32_t kDebugTypeCodeView = 2;

constexpr uint16_t kDllCharDynamicBase = 0x0040;
constexpr uint16_t kDllCharNxCompat = 0x0100;
constexpr uint16_t kDllCharHighEntropyVa = 0x0020;

}  // namespace xbem::pe
