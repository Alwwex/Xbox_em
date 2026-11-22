// kernelx: files, directories and console handles on top of the VFS
// (see runtime/vfs.h for how guest paths map to host paths).

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <vector>

#include "hle/hle_util.h"

namespace xbem::hle {
namespace {

using namespace win;
namespace fs = std::filesystem;

constexpr DWORD GENERIC_READ = 0x80000000;
constexpr DWORD GENERIC_WRITE = 0x40000000;
constexpr DWORD GENERIC_ALL = 0x10000000;
constexpr DWORD FILE_WRITE_DATA = 0x2;
constexpr DWORD FILE_APPEND_DATA = 0x4;

constexpr DWORD CREATE_NEW = 1;
constexpr DWORD CREATE_ALWAYS = 2;
constexpr DWORD OPEN_EXISTING = 3;
constexpr DWORD OPEN_ALWAYS = 4;
constexpr DWORD TRUNCATE_EXISTING = 5;

constexpr DWORD FILE_FLAG_BACKUP_SEMANTICS = 0x02000000;
constexpr DWORD FILE_ATTRIBUTE_READONLY = 0x1;
constexpr DWORD FILE_ATTRIBUTE_DIRECTORY = 0x10;
constexpr DWORD FILE_ATTRIBUTE_NORMAL = 0x80;
constexpr DWORD INVALID_FILE_ATTRIBUTES = 0xFFFFFFFF;

constexpr DWORD FILE_TYPE_UNKNOWN = 0;
constexpr DWORD FILE_TYPE_DISK = 1;
constexpr DWORD FILE_TYPE_CHAR = 2;

class FileObject final : public rt::KernelObject {
 public:
  ~FileObject() override {
    if (file) std::fclose(file);
  }
  const char* type_name() const override { return "File"; }
  std::mutex mutex;
  std::FILE* file = nullptr;
  fs::path path;
  bool writable = false;
  uint64_t position = 0;
};

class DirectoryObject final : public rt::KernelObject {
 public:
  const char* type_name() const override { return "Directory"; }
  fs::path path;
};

class ConsoleObject final : public rt::KernelObject {
 public:
  explicit ConsoleObject(int s) : stream(s) {}
  const char* type_name() const override { return "Console"; }
  int stream;  // 0 = in, 1 = out, 2 = err
};

class FindObject final : public rt::KernelObject {
 public:
  const char* type_name() const override { return "FindFile"; }
  std::vector<fs::directory_entry> entries;
  size_t next = 0;
  bool read_only = false;
};

std::FILE* open_host(const fs::path& p, const char* mode) {
#if defined(_WIN32)
  std::wstring wmode(mode, mode + std::strlen(mode));
  return _wfopen(p.c_str(), wmode.c_str());
#else
  return std::fopen(p.c_str(), mode);
#endif
}

bool seek_host(std::FILE* f, uint64_t pos) {
#if defined(_WIN32)
  return _fseeki64(f, static_cast<long long>(pos), SEEK_SET) == 0;
#else
  return fseeko(f, static_cast<off_t>(pos), SEEK_SET) == 0;
#endif
}

uint64_t to_filetime(fs::file_time_type t) {
  using namespace std::chrono;
  const auto sys = clock_cast<system_clock>(t);
  const auto ns = duration_cast<nanoseconds>(sys.time_since_epoch()).count();
  return 116444736000000000ull + static_cast<uint64_t>(ns / 100);
}

// ------------------------------------------------------------- open/close

HANDLE create_file(const std::string& guest_path, DWORD access, DWORD disposition, DWORD flags) {
  if (iequals(guest_path, "CONOUT$")) return proc().handles().insert(std::make_shared<ConsoleObject>(1));
  if (iequals(guest_path, "CONIN$")) return proc().handles().insert(std::make_shared<ConsoleObject>(0));

  auto r = proc().vfs().resolve(std::string_view(guest_path));
  if (!r) {
    XBEM_DEBUG("kernelx", "CreateFile({}): no mount for this path", guest_path);
    set_last_error(ERROR_PATH_NOT_FOUND);
    return INVALID_HANDLE_VALUE;
  }
  const bool want_write = access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA);
  std::error_code ec;
  const bool exists = fs::exists(r->host_path, ec);
  XBEM_TRACE("kernelx", "CreateFile({}) -> {} (exists={}, write={})", guest_path, r->host_path.string(), exists,
             want_write);

  if (exists && fs::is_directory(r->host_path, ec)) {
    if (!(flags & FILE_FLAG_BACKUP_SEMANTICS)) {
      set_last_error(ERROR_ACCESS_DENIED);
      return INVALID_HANDLE_VALUE;
    }
    auto d = std::make_shared<DirectoryObject>();
    d->path = r->host_path;
    return proc().handles().insert(d);
  }

  const bool creates = disposition == CREATE_NEW || disposition == CREATE_ALWAYS ||
                       disposition == TRUNCATE_EXISTING || (disposition == OPEN_ALWAYS && !exists);
  if (r->read_only && (want_write || creates)) {
    XBEM_DEBUG("kernelx", "CreateFile({}): write access to read-only package denied", guest_path);
    set_last_error(ERROR_ACCESS_DENIED);
    return INVALID_HANDLE_VALUE;
  }

  const char* mode = nullptr;
  DWORD success_error = ERROR_SUCCESS;
  switch (disposition) {
    case CREATE_NEW:
      if (exists) {
        set_last_error(ERROR_FILE_EXISTS);
        return INVALID_HANDLE_VALUE;
      }
      mode = "w+b";
      break;
    case CREATE_ALWAYS:
      mode = "w+b";
      if (exists) success_error = ERROR_ALREADY_EXISTS;
      break;
    case OPEN_EXISTING:
      if (!exists) {
        set_last_error(fs::exists(r->host_path.parent_path(), ec) ? ERROR_FILE_NOT_FOUND : ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
      }
      mode = want_write ? "r+b" : "rb";
      break;
    case OPEN_ALWAYS:
      mode = exists ? (want_write ? "r+b" : "rb") : "w+b";
      if (exists) success_error = ERROR_ALREADY_EXISTS;
      break;
    case TRUNCATE_EXISTING:
      if (!exists) {
        set_last_error(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
      }
      mode = "w+b";
      break;
    default:
      set_last_error(ERROR_INVALID_PARAMETER);
      return INVALID_HANDLE_VALUE;
  }

  std::FILE* f = open_host(r->host_path, mode);
  if (!f) {
    set_last_error(fs::exists(r->host_path.parent_path(), ec) ? ERROR_ACCESS_DENIED : ERROR_PATH_NOT_FOUND);
    return INVALID_HANDLE_VALUE;
  }
  auto obj = std::make_shared<FileObject>();
  obj->file = f;
  obj->path = r->host_path;
  obj->writable = mode[0] == 'w' || mode[1] == '+';
  set_last_error(success_error);
  return proc().handles().insert(obj);
}

HANDLE XBEM_GUEST_ABI hle_CreateFileW(LPCWSTR name, DWORD access, DWORD, void*, DWORD disposition, DWORD flags,
                                      HANDLE) {
  return create_file(narrow(name), access, disposition, flags);
}

HANDLE XBEM_GUEST_ABI hle_CreateFileA(LPCSTR name, DWORD access, DWORD, void*, DWORD disposition, DWORD flags,
                                      HANDLE) {
  return create_file(narrow(name), access, disposition, flags);
}

// CREATEFILE2_EXTENDED_PARAMETERS: { DWORD dwSize; DWORD dwFileAttributes; DWORD dwFileFlags; ... }
HANDLE XBEM_GUEST_ABI hle_CreateFile2(LPCWSTR name, DWORD access, DWORD, DWORD disposition, const DWORD* ext) {
  const DWORD flags = ext && ext[0] >= 12 ? ext[2] : 0;
  return create_file(narrow(name), access, disposition, flags);
}

// --------------------------------------------------------------- I/O

struct Overlapped {
  uint64_t internal;
  uint64_t internal_high;
  uint32_t offset;
  uint32_t offset_high;
  uint64_t event;
};

void complete_overlapped(Overlapped* ov, uint64_t bytes) {
  ov->internal = 0;  // STATUS_SUCCESS
  ov->internal_high = bytes;
  if (ov->event) {
    if (auto e = proc().handles().get_as<rt::EventObject>(ov->event)) e->set();
  }
}

BOOL XBEM_GUEST_ABI hle_ReadFile(HANDLE h, void* buffer, DWORD count, DWORD* read, Overlapped* ov) {
  auto obj = proc().handles().get(h);
  if (read) *read = 0;
  if (std::dynamic_pointer_cast<ConsoleObject>(obj)) return TRUE;  // no console input
  auto f = std::dynamic_pointer_cast<FileObject>(obj);
  if (!f) {
    set_last_error(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  std::lock_guard lock(f->mutex);
  const uint64_t offset = ov ? (static_cast<uint64_t>(ov->offset_high) << 32 | ov->offset) : f->position;
  size_t got = 0;
  if (seek_host(f->file, offset)) got = std::fread(buffer, 1, count, f->file);
  if (ov) complete_overlapped(ov, got);
  else f->position += got;
  if (read) *read = static_cast<DWORD>(got);
  return TRUE;  // reading at EOF succeeds with 0 bytes
}

BOOL XBEM_GUEST_ABI hle_WriteFile(HANDLE h, const void* buffer, DWORD count, DWORD* written, Overlapped* ov) {
  auto obj = proc().handles().get(h);
  if (written) *written = 0;
  if (auto c = std::dynamic_pointer_cast<ConsoleObject>(obj)) {
    log::guest_output(std::string_view(static_cast<const char*>(buffer), count));
    if (written) *written = count;
    return TRUE;
  }
  auto f = std::dynamic_pointer_cast<FileObject>(obj);
  if (!f) {
    set_last_error(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  if (!f->writable) {
    set_last_error(ERROR_ACCESS_DENIED);
    return FALSE;
  }
  std::lock_guard lock(f->mutex);
  const uint64_t offset = ov ? (static_cast<uint64_t>(ov->offset_high) << 32 | ov->offset) : f->position;
  size_t put = 0;
  if (seek_host(f->file, offset)) put = std::fwrite(buffer, 1, count, f->file);
  if (ov) complete_overlapped(ov, put);
  else f->position += put;
  if (written) *written = static_cast<DWORD>(put);
  return put == count ? TRUE : FALSE;
}

BOOL XBEM_GUEST_ABI hle_GetOverlappedResult(HANDLE, Overlapped* ov, DWORD* transferred, BOOL) {
  if (!ov) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  if (transferred) *transferred = static_cast<DWORD>(ov->internal_high);
  return TRUE;  // all I/O completes synchronously
}

BOOL XBEM_GUEST_ABI hle_GetOverlappedResultEx(HANDLE h, Overlapped* ov, DWORD* transferred, DWORD, BOOL) {
  return hle_GetOverlappedResult(h, ov, transferred, TRUE);
}

std::shared_ptr<FileObject> file_from(HANDLE h) {
  auto f = proc().handles().get_as<FileObject>(h);
  if (!f) set_last_error(ERROR_INVALID_HANDLE);
  return f;
}

bool file_size(FileObject& f, uint64_t& size) {
  std::fflush(f.file);
  std::error_code ec;
  size = fs::file_size(f.path, ec);
  return !ec;
}

BOOL XBEM_GUEST_ABI hle_GetFileSizeEx(HANDLE h, LONGLONG* size) {
  auto f = file_from(h);
  uint64_t s = 0;
  if (!f || !size) return FALSE;
  std::lock_guard lock(f->mutex);
  if (!file_size(*f, s)) return FALSE;
  *size = static_cast<LONGLONG>(s);
  return TRUE;
}

DWORD XBEM_GUEST_ABI hle_GetFileSize(HANDLE h, DWORD* high) {
  auto f = file_from(h);
  uint64_t s = 0;
  if (!f) return 0xFFFFFFFF;
  std::lock_guard lock(f->mutex);
  if (!file_size(*f, s)) return 0xFFFFFFFF;
  if (high) *high = static_cast<DWORD>(s >> 32);
  return static_cast<DWORD>(s);
}

bool move_pointer(FileObject& f, int64_t distance, DWORD method, uint64_t& out) {
  int64_t base = 0;
  if (method == 1) base = static_cast<int64_t>(f.position);
  else if (method == 2) {
    uint64_t s = 0;
    if (!file_size(f, s)) return false;
    base = static_cast<int64_t>(s);
  } else if (method != 0) return false;
  if (base + distance < 0) return false;
  f.position = static_cast<uint64_t>(base + distance);
  out = f.position;
  return true;
}

BOOL XBEM_GUEST_ABI hle_SetFilePointerEx(HANDLE h, LONGLONG distance, LONGLONG* new_pos, DWORD method) {
  auto f = file_from(h);
  if (!f) return FALSE;
  std::lock_guard lock(f->mutex);
  uint64_t pos = 0;
  if (!move_pointer(*f, distance, method, pos)) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  if (new_pos) *new_pos = static_cast<LONGLONG>(pos);
  return TRUE;
}

DWORD XBEM_GUEST_ABI hle_SetFilePointer(HANDLE h, LONG distance, LONG* high, DWORD method) {
  auto f = file_from(h);
  if (!f) return 0xFFFFFFFF;
  std::lock_guard lock(f->mutex);
  const int64_t d = high ? (static_cast<int64_t>(*high) << 32) | static_cast<uint32_t>(distance) : distance;
  uint64_t pos = 0;
  if (!move_pointer(*f, d, method, pos)) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return 0xFFFFFFFF;
  }
  if (high) *high = static_cast<LONG>(pos >> 32);
  set_last_error(ERROR_SUCCESS);
  return static_cast<DWORD>(pos);
}

BOOL XBEM_GUEST_ABI hle_FlushFileBuffers(HANDLE h) {
  auto f = file_from(h);
  if (!f) return FALSE;
  std::lock_guard lock(f->mutex);
  std::fflush(f->file);
  return TRUE;
}

BOOL XBEM_GUEST_ABI hle_SetEndOfFile(HANDLE h) {
  auto f = file_from(h);
  if (!f) return FALSE;
  std::lock_guard lock(f->mutex);
  if (!f->writable) {
    set_last_error(ERROR_ACCESS_DENIED);
    return FALSE;
  }
  std::fflush(f->file);
  std::error_code ec;
  fs::resize_file(f->path, f->position, ec);
  return ec ? FALSE : TRUE;
}

DWORD XBEM_GUEST_ABI hle_GetFileType(HANDLE h) {
  auto obj = proc().handles().get(h);
  if (std::dynamic_pointer_cast<ConsoleObject>(obj)) return FILE_TYPE_CHAR;
  if (std::dynamic_pointer_cast<FileObject>(obj) || std::dynamic_pointer_cast<DirectoryObject>(obj))
    return FILE_TYPE_DISK;
  set_last_error(ERROR_INVALID_HANDLE);
  return FILE_TYPE_UNKNOWN;
}

HANDLE XBEM_GUEST_ABI hle_GetStdHandle(DWORD which) {
  static HANDLE handles[3] = {};
  static std::once_flag once;
  std::call_once(once, [] {
    for (int i = 0; i < 3; ++i) handles[i] = proc().handles().insert(std::make_shared<ConsoleObject>(i));
  });
  switch (which) {
    case static_cast<DWORD>(-10): return handles[0];
    case static_cast<DWORD>(-11): return handles[1];
    case static_cast<DWORD>(-12): return handles[2];
    default:
      set_last_error(ERROR_INVALID_HANDLE);
      return INVALID_HANDLE_VALUE;
  }
}

// ----------------------------------------------------------- attributes

DWORD attributes_of(const fs::path& p, bool read_only_mount) {
  std::error_code ec;
  if (fs::is_directory(p, ec)) return FILE_ATTRIBUTE_DIRECTORY;
  return read_only_mount ? FILE_ATTRIBUTE_READONLY : FILE_ATTRIBUTE_NORMAL;
}

DWORD get_attributes(const std::string& guest_path) {
  auto r = proc().vfs().resolve(std::string_view(guest_path));
  std::error_code ec;
  if (!r || !fs::exists(r->host_path, ec)) {
    set_last_error(ERROR_FILE_NOT_FOUND);
    return INVALID_FILE_ATTRIBUTES;
  }
  return attributes_of(r->host_path, r->read_only);
}

DWORD XBEM_GUEST_ABI hle_GetFileAttributesW(LPCWSTR name) { return get_attributes(narrow(name)); }
DWORD XBEM_GUEST_ABI hle_GetFileAttributesA(LPCSTR name) { return get_attributes(narrow(name)); }

void fill_times_and_size(uint8_t* out, const fs::path& p) {
  std::error_code ec;
  const uint64_t ft = to_filetime(fs::last_write_time(p, ec));
  const uint64_t size = fs::is_regular_file(p, ec) ? fs::file_size(p, ec) : 0;
  const DWORD hi = static_cast<DWORD>(size >> 32), lo = static_cast<DWORD>(size);
  std::memcpy(out + 0, &ft, 8);   // creation
  std::memcpy(out + 8, &ft, 8);   // last access
  std::memcpy(out + 16, &ft, 8);  // last write
  std::memcpy(out + 24, &hi, 4);
  std::memcpy(out + 28, &lo, 4);
}

BOOL XBEM_GUEST_ABI hle_GetFileAttributesExW(LPCWSTR name, DWORD level, uint8_t* data) {
  if (level != 0 || !data) {
    set_last_error(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  auto r = proc().vfs().resolve(guest_wstr(name));
  std::error_code ec;
  if (!r || !fs::exists(r->host_path, ec)) {
    set_last_error(ERROR_FILE_NOT_FOUND);
    return FALSE;
  }
  // WIN32_FILE_ATTRIBUTE_DATA: DWORD attrs; FILETIME x3; DWORD size_high; DWORD size_low
  const DWORD attrs = attributes_of(r->host_path, r->read_only);
  std::memcpy(data, &attrs, 4);
  fill_times_and_size(data + 4, r->host_path);
  return TRUE;
}

std::optional<rt::Vfs::Resolved> resolve_writable(const std::string& guest_path) {
  auto r = proc().vfs().resolve(std::string_view(guest_path));
  if (!r) {
    set_last_error(ERROR_PATH_NOT_FOUND);
    return std::nullopt;
  }
  if (r->read_only) {
    set_last_error(ERROR_ACCESS_DENIED);
    return std::nullopt;
  }
  return r;
}

BOOL XBEM_GUEST_ABI hle_DeleteFileW(LPCWSTR name) {
  auto r = resolve_writable(narrow(name));
  if (!r) return FALSE;
  std::error_code ec;
  if (!fs::is_regular_file(r->host_path, ec) || !fs::remove(r->host_path, ec)) {
    set_last_error(ERROR_FILE_NOT_FOUND);
    return FALSE;
  }
  return TRUE;
}

BOOL XBEM_GUEST_ABI hle_CreateDirectoryW(LPCWSTR name, void*) {
  auto r = resolve_writable(narrow(name));
  if (!r) return FALSE;
  std::error_code ec;
  if (fs::exists(r->host_path, ec)) {
    set_last_error(ERROR_ALREADY_EXISTS);
    return FALSE;
  }
  if (!fs::create_directory(r->host_path, ec)) {
    set_last_error(ERROR_PATH_NOT_FOUND);
    return FALSE;
  }
  return TRUE;
}

BOOL XBEM_GUEST_ABI hle_CreateDirectoryA(LPCSTR name, void* sa) {
  const std::u16string w = utf8_to_utf16(guest_cstr(name));
  return hle_CreateDirectoryW(w.c_str(), sa);
}

BOOL XBEM_GUEST_ABI hle_RemoveDirectoryW(LPCWSTR name) {
  auto r = resolve_writable(narrow(name));
  if (!r) return FALSE;
  std::error_code ec;
  if (!fs::is_directory(r->host_path, ec) || !fs::remove(r->host_path, ec)) {
    set_last_error(ERROR_PATH_NOT_FOUND);
    return FALSE;
  }
  return TRUE;
}

// ------------------------------------------------------------ find files

template <class Char>
void fill_find_data(uint8_t* out, const fs::directory_entry& e, bool read_only) {
  const size_t total = sizeof(Char) == 2 ? 592 : 320;  // WIN32_FIND_DATAW / WIN32_FIND_DATAA
  std::memset(out, 0, total);
  const DWORD attrs = attributes_of(e.path(), read_only);
  std::memcpy(out, &attrs, 4);
  fill_times_and_size(out + 4, e.path());
  const std::string name = e.path().filename().string();
  auto* fname = reinterpret_cast<Char*>(out + 44);
  if constexpr (sizeof(Char) == 2) {
    const std::u16string w = utf8_to_utf16(name);
    const size_t n = std::min<size_t>(w.size(), 259);
    std::memcpy(fname, w.data(), n * 2);
  } else {
    std::memcpy(fname, name.data(), std::min<size_t>(name.size(), 259));
  }
}

template <class Char>
HANDLE find_first(const std::string& pattern_path, uint8_t* data) {
  const size_t cut = pattern_path.find_last_of("\\/");
  const std::string dir = cut == std::string::npos ? std::string() : pattern_path.substr(0, cut + 1);
  const std::string pattern = cut == std::string::npos ? pattern_path : pattern_path.substr(cut + 1);
  auto r = proc().vfs().resolve(std::string_view(dir.empty() ? std::string(".") : dir));
  std::error_code ec;
  if (!r || !fs::is_directory(r->host_path, ec)) {
    set_last_error(ERROR_PATH_NOT_FOUND);
    return INVALID_HANDLE_VALUE;
  }
  auto find = std::make_shared<FindObject>();
  find->read_only = r->read_only;
  for (fs::directory_iterator it(r->host_path, ec), end; !ec && it != end; it.increment(ec))
    if (wildcard_match_icase(pattern, it->path().filename().string())) find->entries.push_back(*it);
  if (find->entries.empty()) {
    set_last_error(ERROR_FILE_NOT_FOUND);
    return INVALID_HANDLE_VALUE;
  }
  fill_find_data<Char>(data, find->entries[0], find->read_only);
  find->next = 1;
  return proc().handles().insert(find);
}

template <class Char>
BOOL find_next(HANDLE h, uint8_t* data) {
  auto find = proc().handles().get_as<FindObject>(h);
  if (!find) {
    set_last_error(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  if (find->next >= find->entries.size()) {
    set_last_error(ERROR_NO_MORE_FILES);
    return FALSE;
  }
  fill_find_data<Char>(data, find->entries[find->next++], find->read_only);
  return TRUE;
}

HANDLE XBEM_GUEST_ABI hle_FindFirstFileW(LPCWSTR name, uint8_t* data) { return find_first<char16_t>(narrow(name), data); }
HANDLE XBEM_GUEST_ABI hle_FindFirstFileA(LPCSTR name, uint8_t* data) { return find_first<char>(narrow(name), data); }
HANDLE XBEM_GUEST_ABI hle_FindFirstFileExW(LPCWSTR name, DWORD, uint8_t* data, DWORD, void*, DWORD) {
  return find_first<char16_t>(narrow(name), data);
}
BOOL XBEM_GUEST_ABI hle_FindNextFileW(HANDLE h, uint8_t* data) { return find_next<char16_t>(h, data); }
BOOL XBEM_GUEST_ABI hle_FindNextFileA(HANDLE h, uint8_t* data) { return find_next<char>(h, data); }

BOOL XBEM_GUEST_ABI hle_FindClose(HANDLE h) {
  if (!proc().handles().get_as<FindObject>(h)) {
    set_last_error(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  proc().handles().close(h);
  return TRUE;
}

// ---------------------------------------------------- directories/paths

std::u16string drive_root(char drive) { return std::u16string{static_cast<char16_t>(drive), u':', u'\\'}; }

DWORD XBEM_GUEST_ABI hle_GetCurrentDirectoryW(DWORD size, LPWSTR buf) {
  return copy_out_string<char16_t>(drive_root(proc().vfs().default_drive()), buf, size);
}

BOOL XBEM_GUEST_ABI hle_SetCurrentDirectoryW(LPCWSTR path) {
  XBEM_WARN_ONCE("kernelx", "SetCurrentDirectory({}) ignored: the working directory is the package root",
                 narrow(path));
  return TRUE;
}

DWORD XBEM_GUEST_ABI hle_GetTempPathW(DWORD size, LPWSTR buf) {
  return copy_out_string<char16_t>(drive_root('T'), buf, size);
}

DWORD XBEM_GUEST_ABI hle_GetFullPathNameW(LPCWSTR name, DWORD size, LPWSTR buf, LPWSTR* file_part) {
  std::u16string p(guest_wstr(name));
  for (auto& c : p)
    if (c == u'/') c = u'\\';
  const bool absolute = p.size() >= 2 && p[1] == u':';
  if (!absolute) {
    const std::u16string root = drive_root(proc().vfs().default_drive());
    p = (!p.empty() && p[0] == u'\\') ? root.substr(0, 2) + p : root + p;
  }
  const DWORD n = copy_out_string<char16_t>(p, buf, size);
  if (file_part && buf && n < size) {
    const size_t cut = p.find_last_of(u'\\');
    *file_part = (cut == std::u16string::npos || cut + 1 >= p.size()) ? nullptr : buf + cut + 1;
  }
  return n;
}

}  // namespace

void add_kernelx_file_exports(std::vector<ExportDef>& out) {
  out.insert(out.end(), {
      XBEM_HLE(CreateFileW), XBEM_HLE(CreateFileA), XBEM_HLE(CreateFile2), XBEM_HLE(ReadFile), XBEM_HLE(WriteFile),
      XBEM_HLE(GetOverlappedResult), XBEM_HLE(GetOverlappedResultEx), XBEM_HLE(GetFileSizeEx), XBEM_HLE(GetFileSize),
      XBEM_HLE(SetFilePointerEx), XBEM_HLE(SetFilePointer), XBEM_HLE(FlushFileBuffers), XBEM_HLE(SetEndOfFile),
      XBEM_HLE(GetFileType), XBEM_HLE(GetStdHandle), XBEM_HLE(GetFileAttributesW), XBEM_HLE(GetFileAttributesA),
      XBEM_HLE(GetFileAttributesExW), XBEM_HLE(DeleteFileW), XBEM_HLE(CreateDirectoryW), XBEM_HLE(CreateDirectoryA),
      XBEM_HLE(RemoveDirectoryW), XBEM_HLE(FindFirstFileW), XBEM_HLE(FindFirstFileA), XBEM_HLE(FindFirstFileExW),
      XBEM_HLE(FindNextFileW), XBEM_HLE(FindNextFileA), XBEM_HLE(FindClose), XBEM_HLE(GetCurrentDirectoryW),
      XBEM_HLE(SetCurrentDirectoryW), XBEM_HLE(GetTempPathW), XBEM_HLE(GetFullPathNameW),
  });
}

}  // namespace xbem::hle
