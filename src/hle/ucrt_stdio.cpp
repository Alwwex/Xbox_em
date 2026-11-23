// UCRT: printf/scanf engines (Windows conventions: %I64d, %ls, 16-bit wide
// strings, MSVC NaN spelling), FILE streams on top of the VFS and the
// low-level _open/_read/_write file descriptor API.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#else
#include <io.h>
#endif

#include "hle/heap.h"
#include "hle/ucrt.h"

namespace xbem::hle {

namespace crt {

// ============================================================== printf

namespace {

enum class Len { None, hh, h, l, ll, L, j, z, t, I32, I64, I, w };

struct Spec {
  bool left = false, plus = false, space = false, alt = false, zero = false;
  int width = 0;
  int precision = -1;
  Len len = Len::None;
  char conv = 0;
};

bool is_64bit(Len l) { return l == Len::ll || l == Len::L || l == Len::j || l == Len::z || l == Len::t || l == Len::I64 || l == Len::I; }

std::string host_spec(const Spec& s, const char* length, char conv) {
  std::string f = "%";
  if (s.left) f += '-';
  if (s.plus) f += '+';
  if (s.space) f += ' ';
  if (s.alt) f += '#';
  if (s.zero) f += '0';
  if (s.width) f += std::to_string(s.width);
  if (s.precision >= 0) f += "." + std::to_string(s.precision);
  f += length;
  f += conv;
  return f;
}

template <class... A>
std::string host_format(const std::string& spec, A... a) {
  const int n = std::snprintf(nullptr, 0, spec.c_str(), a...);
  if (n <= 0) return {};
  std::string out(static_cast<size_t>(n) + 1, '\0');
  std::snprintf(out.data(), out.size(), spec.c_str(), a...);
  out.resize(static_cast<size_t>(n));
  return out;
}

template <class Out>
void append_ascii(std::basic_string<Out>& out, std::string_view s) {
  for (char c : s) out.push_back(static_cast<Out>(static_cast<unsigned char>(c)));
}

template <class Out>
void pad(std::basic_string<Out>& out, const std::basic_string<Out>& body, const Spec& s) {
  const size_t w = static_cast<size_t>(std::max(s.width, 0));
  if (body.size() >= w) {
    out += body;
    return;
  }
  const size_t fill = w - body.size();
  if (s.left) {
    out += body;
    out.append(fill, static_cast<Out>(' '));
  } else {
    out.append(fill, static_cast<Out>(s.zero ? '0' : ' '));
    out += body;
  }
}

template <class Out>
std::basic_string<Out> from_narrow(std::string_view s) {
  if constexpr (sizeof(Out) == 1) return std::string(s);
  else return to_wide(s);
}

template <class Out>
std::basic_string<Out> from_wide(std::u16string_view s) {
  if constexpr (sizeof(Out) == 1) return to_narrow(s);
  else return std::u16string(s);
}

template <class In, class Out>
std::basic_string<Out> format_impl(const In* f, ArgReader& args, uint64_t options) {
  constexpr bool kWideFn = sizeof(In) == 2;
  std::basic_string<Out> out;
  if (!f) return out;
  while (*f) {
    if (*f != '%') {
      out.push_back(static_cast<Out>(*f++));
      continue;
    }
    ++f;
    if (*f == '%') {
      out.push_back(static_cast<Out>('%'));
      ++f;
      continue;
    }
    Spec s;
    for (bool more = true; more;) {
      switch (*f) {
        case '-': s.left = true; ++f; break;
        case '+': s.plus = true; ++f; break;
        case ' ': s.space = true; ++f; break;
        case '#': s.alt = true; ++f; break;
        case '0': s.zero = true; ++f; break;
        case '\'': ++f; break;
        default: more = false;
      }
    }
    if (*f == '*') {
      int w = static_cast<int32_t>(args.next_u64());
      if (w < 0) {
        s.left = true;
        w = -w;
      }
      s.width = w;
      ++f;
    } else {
      while (*f >= '0' && *f <= '9') s.width = s.width * 10 + (*f++ - '0');
    }
    if (*f == '.') {
      ++f;
      s.precision = 0;
      if (*f == '*') {
        const int p = static_cast<int32_t>(args.next_u64());
        s.precision = p < 0 ? -1 : p;
        ++f;
      } else {
        while (*f >= '0' && *f <= '9') s.precision = s.precision * 10 + (*f++ - '0');
      }
    }
    switch (*f) {
      case 'h':
        ++f;
        if (*f == 'h') { s.len = Len::hh; ++f; } else s.len = Len::h;
        break;
      case 'l':
        ++f;
        if (*f == 'l') { s.len = Len::ll; ++f; } else s.len = Len::l;
        break;
      case 'L': s.len = Len::L; ++f; break;
      case 'j': s.len = Len::j; ++f; break;
      case 'z': s.len = Len::z; ++f; break;
      case 't': s.len = Len::t; ++f; break;
      case 'w': s.len = Len::w; ++f; break;
      case 'I':
        if (f[1] == '3' && f[2] == '2') { s.len = Len::I32; f += 3; }
        else if (f[1] == '6' && f[2] == '4') { s.len = Len::I64; f += 3; }
        else { s.len = Len::I; ++f; }
        break;
      default: break;
    }
    if (!*f) break;
    s.conv = static_cast<char>(*f++);

    switch (s.conv) {
      case 'd':
      case 'i': {
        const uint64_t raw = args.next_u64();
        long long v;
        if (is_64bit(s.len)) v = static_cast<int64_t>(raw);
        else if (s.len == Len::hh) v = static_cast<int8_t>(raw);
        else if (s.len == Len::h) v = static_cast<int16_t>(raw);
        else v = static_cast<int32_t>(raw);
        append_ascii(out, host_format(host_spec(s, "ll", 'd'), v));
        break;
      }
      case 'u':
      case 'o':
      case 'x':
      case 'X': {
        const uint64_t raw = args.next_u64();
        unsigned long long v;
        if (is_64bit(s.len)) v = raw;
        else if (s.len == Len::hh) v = static_cast<uint8_t>(raw);
        else if (s.len == Len::h) v = static_cast<uint16_t>(raw);
        else v = static_cast<uint32_t>(raw);
        append_ascii(out, host_format(host_spec(s, "ll", s.conv), v));
        break;
      }
      case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
        const double v = args.next_double();
        if (std::isnan(v)) {
          const bool upper = s.conv >= 'A' && s.conv <= 'Z';
          std::string body = std::signbit(v) ? "-nan(ind)" : "nan";
          if (upper)
            for (char& c : body) c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
          Spec plain = s;
          plain.zero = false;
          pad(out, from_narrow<Out>(body), plain);
        } else {
          append_ascii(out, host_format(host_spec(s, "", s.conv), v));
        }
        break;
      }
      case 'c':
      case 'C': {
        const uint64_t raw = args.next_u64();
        bool wide_arg;
        if (s.len == Len::l || s.len == Len::w) wide_arg = true;
        else if (s.len == Len::h) wide_arg = false;
        else if (kWideFn) wide_arg = (options & kPrintfLegacyWideSpecifiers) ? s.conv == 'c' : s.conv == 'C';
        else wide_arg = s.conv == 'C';
        std::basic_string<Out> body;
        if (wide_arg) body = from_wide<Out>(std::u16string(1, static_cast<char16_t>(raw)));
        else body = from_narrow<Out>(std::string(1, static_cast<char>(raw)));
        pad(out, body, s);
        break;
      }
      case 's':
      case 'S': {
        const uint64_t ptr = args.next_u64();
        bool wide_arg;
        if (s.len == Len::l || s.len == Len::w) wide_arg = true;
        else if (s.len == Len::h) wide_arg = false;
        else if (kWideFn) wide_arg = (options & kPrintfLegacyWideSpecifiers) ? s.conv == 's' : s.conv == 'S';
        else wide_arg = s.conv == 'S';
        std::basic_string<Out> body;
        const size_t max = s.precision >= 0 ? static_cast<size_t>(s.precision) : ~size_t(0);
        if (!ptr) {
          body = from_narrow<Out>(std::string("(null)").substr(0, max));
        } else if (wide_arg) {
          const auto* p = reinterpret_cast<const char16_t*>(ptr);
          body = from_wide<Out>(std::u16string_view(p, bounded_strlen16(p, max)));
        } else {
          const auto* p = reinterpret_cast<const char*>(ptr);
          body = from_narrow<Out>(std::string_view(p, bounded_strlen(p, max)));
        }
        pad(out, body, s);
        break;
      }
      case 'p': {
        const uint64_t v = args.next_u64();
        Spec plain = s;
        plain.zero = false;
        pad(out, from_narrow<Out>(host_format("%016llX", static_cast<unsigned long long>(v))), plain);
        break;
      }
      case 'n':
      case 'Z':
        args.next_u64();  // %n is disabled in the UCRT; ANSI_STRING is not supported
        break;
      default:
        out.push_back(static_cast<Out>('%'));
        out.push_back(static_cast<Out>(s.conv));
        break;
    }
  }
  return out;
}

}  // namespace

std::string format_narrow(const char* format, ArgReader& args, uint64_t options) {
  return format_impl<char, char>(format, args, options);
}

std::u16string format_wide(const char16_t* format, ArgReader& args, uint64_t options) {
  return format_impl<char16_t, char16_t>(format, args, options);
}

// =============================================================== scanf

namespace {

template <class C>
bool is_space(C c) {
  return c == ' ' || (c >= '\t' && c <= '\r');
}

template <class C>
int scan_impl(const C* in, const C* f, ArgReader& args, bool secure) {
  constexpr bool kWideFn = sizeof(C) == 2;
  int assigned = 0;
  bool any_conversion = false;
  while (*f) {
    if (is_space(*f)) {
      while (is_space(*f)) ++f;
      while (is_space(*in)) ++in;
      continue;
    }
    if (*f != '%') {
      if (*in != *f) break;
      ++in;
      ++f;
      continue;
    }
    ++f;
    if (*f == '%') {
      while (is_space(*in)) ++in;
      if (*in != '%') break;
      ++in;
      ++f;
      continue;
    }
    bool suppress = false;
    if (*f == '*') {
      suppress = true;
      ++f;
    }
    size_t width = 0;
    while (*f >= '0' && *f <= '9') width = width * 10 + static_cast<size_t>(*f++ - '0');
    Len len = Len::None;
    switch (*f) {
      case 'h': ++f; if (*f == 'h') { len = Len::hh; ++f; } else len = Len::h; break;
      case 'l': ++f; if (*f == 'l') { len = Len::ll; ++f; } else len = Len::l; break;
      case 'L': len = Len::L; ++f; break;
      case 'j': len = Len::j; ++f; break;
      case 'z': len = Len::z; ++f; break;
      case 't': len = Len::t; ++f; break;
      case 'w': len = Len::w; ++f; break;
      case 'I':
        if (f[1] == '6' && f[2] == '4') { len = Len::I64; f += 3; }
        else if (f[1] == '3' && f[2] == '2') { len = Len::I32; f += 3; }
        else { len = Len::I; ++f; }
        break;
      default: break;
    }
    const C conv = *f++;
    if (!conv) break;
    if (conv != 'c' && conv != 'C' && conv != '[' && conv != 'n') while (is_space(*in)) ++in;
    if (conv == 'n') {
      if (!suppress) *args.next_ptr<int32_t>() = 0;  // position tracking omitted
      continue;
    }
    if (!*in) return any_conversion ? assigned : -1;
    any_conversion = true;

    // Collect the field text.
    const size_t limit = width ? width : ~size_t(0);
    if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'o' || conv == 'x' || conv == 'X' || conv == 'p') {
      std::string field;
      for (size_t k = 0; k < limit && in[k] && static_cast<uint32_t>(in[k]) < 0x80; ++k) field.push_back(static_cast<char>(in[k]));
      const int base = conv == 'o' ? 8 : (conv == 'x' || conv == 'X' || conv == 'p') ? 16 : conv == 'i' ? 0 : 10;
      char* end = nullptr;
      const bool is_signed = conv == 'd' || conv == 'i';
      const unsigned long long uv = is_signed ? static_cast<unsigned long long>(std::strtoll(field.c_str(), &end, base))
                                              : std::strtoull(field.c_str(), &end, base);
      if (end == field.c_str()) break;
      in += end - field.c_str();
      if (suppress) continue;
      void* target = args.next_ptr<void>();
      const bool wide64 = is_64bit(len) || conv == 'p';
      if (wide64) std::memcpy(target, &uv, 8);
      else if (len == Len::hh) *static_cast<uint8_t*>(target) = static_cast<uint8_t>(uv);
      else if (len == Len::h) *static_cast<uint16_t*>(target) = static_cast<uint16_t>(uv);
      else *static_cast<uint32_t*>(target) = static_cast<uint32_t>(uv);
      ++assigned;
      continue;
    }
    if (conv == 'e' || conv == 'E' || conv == 'f' || conv == 'F' || conv == 'g' || conv == 'G' || conv == 'a' ||
        conv == 'A') {
      std::string field;
      for (size_t k = 0; k < limit && in[k] && static_cast<uint32_t>(in[k]) < 0x80; ++k) field.push_back(static_cast<char>(in[k]));
      char* end = nullptr;
      const double v = std::strtod(field.c_str(), &end);
      if (end == field.c_str()) break;
      in += end - field.c_str();
      if (suppress) continue;
      if (len == Len::l || len == Len::L) *args.next_ptr<double>() = v;
      else *args.next_ptr<float>() = static_cast<float>(v);
      ++assigned;
      continue;
    }
    if (conv == 's' || conv == 'S' || conv == 'c' || conv == 'C' || conv == '[') {
      bool wide_target;
      if (len == Len::l || len == Len::w) wide_target = true;
      else if (len == Len::h) wide_target = false;
      else wide_target = kWideFn ? (conv != 'S' && conv != 'C') : (conv == 'S' || conv == 'C');

      std::basic_string<C> set;
      bool negate = false;
      if (conv == '[') {
        if (*f == '^') {
          negate = true;
          ++f;
        }
        if (*f == ']') set.push_back(*f++);
        while (*f && *f != ']') set.push_back(*f++);
        if (*f == ']') ++f;
      }
      auto in_set = [&](C ch) {
        bool found = false;
        for (size_t k = 0; k < set.size(); ++k) {
          if (k + 2 < set.size() && set[k + 1] == '-' && ch >= set[k] && ch <= set[k + 2]) found = true;
          if (set[k] == ch) found = true;
        }
        return found != negate;
      };
      std::basic_string<C> field;
      const bool is_char = conv == 'c' || conv == 'C';
      const size_t max = is_char ? (width ? width : 1) : limit;
      for (size_t k = 0; k < max && *in; ++k, ++in) {
        if (!is_char && conv != '[' && is_space(*in)) break;
        if (conv == '[' && !in_set(*in)) break;
        field.push_back(*in);
      }
      if (field.empty()) break;
      if (suppress) continue;
      void* target = args.next_ptr<void>();
      size_t capacity = ~size_t(0);
      if (secure) capacity = static_cast<uint32_t>(args.next_u64());
      const size_t needed = field.size() + (is_char ? 0 : 1);
      if (needed > capacity) {
        if (capacity && !is_char) {
          if (wide_target) *static_cast<char16_t*>(target) = 0;
          else *static_cast<char*>(target) = 0;
        }
        break;
      }
      if (wide_target) {
        auto* t = static_cast<char16_t*>(target);
        for (size_t k = 0; k < field.size(); ++k) t[k] = static_cast<char16_t>(field[k]);
        if (!is_char) t[field.size()] = 0;
      } else {
        auto* t = static_cast<char*>(target);
        for (size_t k = 0; k < field.size(); ++k) t[k] = static_cast<char>(field[k]);
        if (!is_char) t[field.size()] = 0;
      }
      ++assigned;
      continue;
    }
    break;  // unknown conversion
  }
  return assigned;
}

}  // namespace

int scan_narrow(const char* input, const char* format, ArgReader& args) { return scan_impl(input, format, args, false); }
int scan_wide(const char16_t* input, const char16_t* format, ArgReader& args) { return scan_impl(input, format, args, false); }

// ============================================================= streams

namespace {

std::mutex g_files_mutex;
std::vector<GuestFile*> g_fd_table;  // index = guest fd

GuestFile* std_files() {
  static GuestFile files[3] = {
      {kGuestFileMagic, stdin, 0, kFileStd, nullptr, nullptr, 0, 0},
      {kGuestFileMagic, stdout, 1, kFileStd, nullptr, nullptr, 0, 0},
      {kGuestFileMagic, stderr, 2, kFileStd, nullptr, nullptr, 0, 0},
  };
  return files;
}

int register_fd(GuestFile* f) {
  std::lock_guard lock(g_files_mutex);
  if (g_fd_table.empty()) g_fd_table = {std_files(), std_files() + 1, std_files() + 2};
  for (size_t i = 3; i < g_fd_table.size(); ++i)
    if (!g_fd_table[i]) {
      g_fd_table[i] = f;
      return static_cast<int>(i);
    }
  g_fd_table.push_back(f);
  return static_cast<int>(g_fd_table.size() - 1);
}

std::FILE* open_host_file(const std::filesystem::path& p, const char* mode) {
#if defined(_WIN32)
  std::wstring wm(mode, mode + std::strlen(mode));
  return _wfopen(p.c_str(), wm.c_str());
#else
  return std::fopen(p.c_str(), mode);
#endif
}

}  // namespace

GuestFile* std_stream(unsigned index) { return index < 3 ? std_files() + index : nullptr; }

GuestFile* guest_file(void* f) {
  auto* g = static_cast<GuestFile*>(f);
  if (!g || g->magic != kGuestFileMagic) {
    set_errno(kEBADF);
    return nullptr;
  }
  return g;
}

GuestFile* file_from_fd(int fd) {
  std::lock_guard lock(g_files_mutex);
  if (fd >= 0 && fd < 3) return std_files() + fd;
  if (fd < 0 || static_cast<size_t>(fd) >= g_fd_table.size()) return nullptr;
  return g_fd_table[static_cast<size_t>(fd)];
}

GuestFile* open_guest_file(const std::string& guest_path, const std::string& mode, int* error) {
  // MSVC modes: r w a [+] [b|t] plus extras (c n S R T D N, ccs=...) that do not matter here.
  char base = 0;
  bool plus = false, binary = false;
  for (char c : mode) {
    if (c == ',') break;  // ",ccs=UTF-8" etc.
    if ((c == 'r' || c == 'w' || c == 'a') && !base) base = c;
    else if (c == '+') plus = true;
    else if (c == 'b') binary = true;
  }
  if (!base) {
    *error = kEINVAL;
    return nullptr;
  }
  auto resolved = proc().vfs().resolve(std::string_view(guest_path));
  if (!resolved) {
    *error = kENOENT;
    return nullptr;
  }
  const bool writes = base != 'r' || plus;
  if (writes && resolved->read_only) {
    *error = kEACCES;
    return nullptr;
  }
  std::string host_mode(1, base);
  if (plus) host_mode += '+';
  host_mode += 'b';
  std::FILE* hf = open_host_file(resolved->host_path, host_mode.c_str());
  if (!hf) {
    std::error_code ec;
    *error = std::filesystem::exists(resolved->host_path, ec) ? kEACCES : kENOENT;
    return nullptr;
  }
  auto* f = new GuestFile{kGuestFileMagic, hf, -1, 0, nullptr, nullptr, 0, 0};
  if (!binary) f->flags |= kFileText;
  if (resolved->read_only) f->flags |= kFileReadOnly;
  f->fd = register_fd(f);
  XBEM_TRACE("ucrt", "fopen({}, {}) -> fd {}", guest_path, mode, f->fd);
  return f;
}

int close_guest_file(GuestFile* f) {
  if (f->flags & kFileStd) {
    std::fflush(f->host);
    return 0;
  }
  {
    std::lock_guard lock(g_files_mutex);
    if (f->fd >= 0 && static_cast<size_t>(f->fd) < g_fd_table.size()) g_fd_table[static_cast<size_t>(f->fd)] = nullptr;
  }
  const int r = std::fclose(f->host);
  f->magic = 0;
  delete f;
  return r == 0 ? 0 : -1;
}

size_t write_guest_file(GuestFile* f, const void* data, size_t size) {
  if (f->flags & kFileReadOnly) {
    set_errno(kEBADF);
    return 0;
  }
  return std::fwrite(data, 1, size, f->host);
}

}  // namespace crt

namespace {

using namespace win;
using crt::ArgReader;
using crt::GuestFile;
using crt::GuestVaList;
using crt::set_errno;

// Reads one byte with text-mode CRLF -> LF translation.
int read_char(GuestFile* f) {
  int c = std::fgetc(f->host);
  if (c == '\r' && (f->flags & crt::kFileText)) {
    const int next = std::fgetc(f->host);
    if (next == '\n') return '\n';
    if (next != EOF) std::ungetc(next, f->host);
  }
  return c;
}

size_t read_bytes(GuestFile* f, void* buffer, size_t size) {
  if (!(f->flags & crt::kFileText)) return std::fread(buffer, 1, size, f->host);
  auto* out = static_cast<uint8_t*>(buffer);
  size_t n = 0;
  while (n < size) {
    const int c = read_char(f);
    if (c == EOF) break;
    out[n++] = static_cast<uint8_t>(c);
  }
  return n;
}

// ------------------------------------------------------ sprintf family

template <class C>
int32_t finish_sprintf(uint64_t options, C* buffer, size_t count, const std::basic_string<C>& s) {
  const size_t len = s.size();
  if (!buffer) return count == 0 ? static_cast<int32_t>(len) : -1;
  if (len < count) {
    std::memcpy(buffer, s.data(), len * sizeof(C));
    buffer[len] = 0;
    return static_cast<int32_t>(len);
  }
  if (count == 0) return (options & crt::kPrintfStandardSnprintfBehavior) ? static_cast<int32_t>(len) : -1;
  if (options & crt::kPrintfStandardSnprintfBehavior) {
    std::memcpy(buffer, s.data(), (count - 1) * sizeof(C));
    buffer[count - 1] = 0;
    return static_cast<int32_t>(len);
  }
  std::memcpy(buffer, s.data(), count * sizeof(C));
  if (options & crt::kPrintfLegacyVsprintfNullTermination) buffer[count - 1] = 0;
  return len == count ? static_cast<int32_t>(len) : -1;
}

// sprintf/vsprintf: the caller promises the buffer is large enough.
template <class C>
int32_t finish_unbounded(C* buffer, const std::basic_string<C>& s) {
  if (!buffer) return -1;
  std::memcpy(buffer, s.c_str(), (s.size() + 1) * sizeof(C));
  return static_cast<int32_t>(s.size());
}

template <class C>
int32_t finish_sprintf_s(C* buffer, size_t count, const std::basic_string<C>& s, const char* fn) {
  if (!buffer || count == 0) {
    crt::invalid_parameter(fn);
    set_errno(crt::kEINVAL);
    return -1;
  }
  if (s.size() >= count) {
    buffer[0] = 0;
    crt::invalid_parameter(fn);
    set_errno(crt::kERANGE);
    return -1;
  }
  std::memcpy(buffer, s.data(), s.size() * sizeof(C));
  buffer[s.size()] = 0;
  return static_cast<int32_t>(s.size());
}

template <class C>
int32_t finish_snprintf_s(C* buffer, size_t size, size_t max_count, const std::basic_string<C>& s, const char* fn) {
  if (!buffer || size == 0) {
    crt::invalid_parameter(fn);
    return -1;
  }
  const size_t limit = std::min(max_count, size - 1);
  if (s.size() <= limit) {
    std::memcpy(buffer, s.data(), s.size() * sizeof(C));
    buffer[s.size()] = 0;
    return static_cast<int32_t>(s.size());
  }
  if (max_count == ~size_t(0) || max_count < size) {
    std::memcpy(buffer, s.data(), limit * sizeof(C));
    buffer[limit] = 0;
    return -1;
  }
  buffer[0] = 0;
  crt::invalid_parameter(fn);
  return -1;
}

int32_t XBEM_GUEST_ABI hle___stdio_common_vsprintf(uint64_t options, char* buffer, SIZE_T count, const char* fmt,
                                                    void*, GuestVaList ap) {
  ArgReader a = crt::args_of(ap);
  const std::string s = crt::format_narrow(fmt, a, options);
  return count == ~SIZE_T(0) ? finish_unbounded(buffer, s) : finish_sprintf(options, buffer, count, s);
}

int32_t XBEM_GUEST_ABI hle___stdio_common_vsprintf_s(uint64_t options, char* buffer, SIZE_T count, const char* fmt,
                                                      void*, GuestVaList ap) {
  ArgReader a = crt::args_of(ap);
  return finish_sprintf_s(buffer, count, crt::format_narrow(fmt, a, options), "sprintf_s");
}

int32_t XBEM_GUEST_ABI hle___stdio_common_vsnprintf_s(uint64_t options, char* buffer, SIZE_T size, SIZE_T max_count,
                                                       const char* fmt, void*, GuestVaList ap) {
  ArgReader a = crt::args_of(ap);
  return finish_snprintf_s(buffer, size, max_count, crt::format_narrow(fmt, a, options), "_snprintf_s");
}

int32_t XBEM_GUEST_ABI hle___stdio_common_vswprintf(uint64_t options, char16_t* buffer, SIZE_T count,
                                                     const char16_t* fmt, void*, GuestVaList ap) {
  ArgReader a = crt::args_of(ap);
  const std::u16string s = crt::format_wide(fmt, a, options);
  return count == ~SIZE_T(0) ? finish_unbounded(buffer, s) : finish_sprintf(options, buffer, count, s);
}

int32_t XBEM_GUEST_ABI hle___stdio_common_vswprintf_s(uint64_t options, char16_t* buffer, SIZE_T count,
                                                       const char16_t* fmt, void*, GuestVaList ap) {
  ArgReader a = crt::args_of(ap);
  return finish_sprintf_s(buffer, count, crt::format_wide(fmt, a, options), "swprintf_s");
}

int32_t XBEM_GUEST_ABI hle___stdio_common_vsnwprintf_s(uint64_t options, char16_t* buffer, SIZE_T size,
                                                        SIZE_T max_count, const char16_t* fmt, void*, GuestVaList ap) {
  ArgReader a = crt::args_of(ap);
  return finish_snprintf_s(buffer, size, max_count, crt::format_wide(fmt, a, options), "_snwprintf_s");
}

int32_t write_formatted(void* stream, const std::string& s) {
  GuestFile* f = crt::guest_file(stream);
  if (!f) return -1;
  const size_t n = crt::write_guest_file(f, s.data(), s.size());
  return n == s.size() ? static_cast<int32_t>(n) : -1;
}

int32_t XBEM_GUEST_ABI hle___stdio_common_vfprintf(uint64_t options, void* stream, const char* fmt, void*,
                                                    GuestVaList ap) {
  ArgReader a = crt::args_of(ap);
  return write_formatted(stream, crt::format_narrow(fmt, a, options));
}

int32_t XBEM_GUEST_ABI hle___stdio_common_vfwprintf(uint64_t options, void* stream, const char16_t* fmt, void*,
                                                     GuestVaList ap) {
  ArgReader a = crt::args_of(ap);
  const std::u16string w = crt::format_wide(fmt, a, options);
  return write_formatted(stream, crt::to_narrow(w)) < 0 ? -1 : static_cast<int32_t>(w.size());
}

int32_t XBEM_GUEST_ABI hle___stdio_common_vsscanf(uint64_t options, const char* input, SIZE_T count, const char* fmt,
                                                   void*, GuestVaList ap) {
  ArgReader a = crt::args_of(ap);
  if (!input || !fmt) return -1;
  std::string bounded(input, count == ~SIZE_T(0) ? bounded_strlen(input) : bounded_strlen(input, count));
  return crt::scan_impl(bounded.c_str(), fmt, a, options & 1 /*_CRT_INTERNAL_SCANF_SECURECRT*/);
}

int32_t XBEM_GUEST_ABI hle___stdio_common_vswscanf(uint64_t options, const char16_t* input, SIZE_T count,
                                                    const char16_t* fmt, void*, GuestVaList ap) {
  ArgReader a = crt::args_of(ap);
  if (!input || !fmt) return -1;
  std::u16string bounded(input, count == ~SIZE_T(0) ? bounded_strlen16(input) : bounded_strlen16(input, count));
  return crt::scan_impl(bounded.c_str(), fmt, a, options & 1);
}

// msvcrt-style variadic entry points (also handy for MinGW-built programs).
int32_t XBEM_GUEST_ABI hle_printf(const char* fmt, ...) {
  GuestVaList ap;
  XBEM_GUEST_VA_START(ap, fmt);
  ArgReader a = crt::args_of(ap);
  const int32_t r = write_formatted(crt::std_stream(1), crt::format_narrow(fmt, a, 0));
  XBEM_GUEST_VA_END(ap);
  return r;
}

int32_t XBEM_GUEST_ABI hle_fprintf(void* stream, const char* fmt, ...) {
  GuestVaList ap;
  XBEM_GUEST_VA_START(ap, fmt);
  ArgReader a = crt::args_of(ap);
  const int32_t r = write_formatted(stream, crt::format_narrow(fmt, a, 0));
  XBEM_GUEST_VA_END(ap);
  return r;
}

int32_t XBEM_GUEST_ABI hle_sprintf(char* buffer, const char* fmt, ...) {
  GuestVaList ap;
  XBEM_GUEST_VA_START(ap, fmt);
  ArgReader a = crt::args_of(ap);
  const int32_t r = finish_unbounded(buffer, crt::format_narrow(fmt, a, 0));
  XBEM_GUEST_VA_END(ap);
  return r;
}

int32_t XBEM_GUEST_ABI hle_snprintf(char* buffer, SIZE_T count, const char* fmt, ...) {
  GuestVaList ap;
  XBEM_GUEST_VA_START(ap, fmt);
  ArgReader a = crt::args_of(ap);
  const int32_t r =
      finish_sprintf(crt::kPrintfStandardSnprintfBehavior, buffer, count, crt::format_narrow(fmt, a, 0));
  XBEM_GUEST_VA_END(ap);
  return r;
}

int32_t XBEM_GUEST_ABI hle__snprintf(char* buffer, SIZE_T count, const char* fmt, ...) {
  GuestVaList ap;
  XBEM_GUEST_VA_START(ap, fmt);
  ArgReader a = crt::args_of(ap);
  const int32_t r = finish_sprintf(0, buffer, count, crt::format_narrow(fmt, a, 0));
  XBEM_GUEST_VA_END(ap);
  return r;
}

int32_t XBEM_GUEST_ABI hle_sscanf(const char* input, const char* fmt, ...) {
  GuestVaList ap;
  XBEM_GUEST_VA_START(ap, fmt);
  ArgReader a = crt::args_of(ap);
  const int32_t r = input && fmt ? crt::scan_narrow(input, fmt, a) : -1;
  XBEM_GUEST_VA_END(ap);
  return r;
}

int32_t XBEM_GUEST_ABI hle_vprintf(const char* fmt, GuestVaList ap) {
  ArgReader a = crt::args_of(ap);
  return write_formatted(crt::std_stream(1), crt::format_narrow(fmt, a, 0));
}
int32_t XBEM_GUEST_ABI hle_vfprintf(void* stream, const char* fmt, GuestVaList ap) {
  ArgReader a = crt::args_of(ap);
  return write_formatted(stream, crt::format_narrow(fmt, a, 0));
}
int32_t XBEM_GUEST_ABI hle_vsprintf(char* buffer, const char* fmt, GuestVaList ap) {
  ArgReader a = crt::args_of(ap);
  return finish_unbounded(buffer, crt::format_narrow(fmt, a, 0));
}
int32_t XBEM_GUEST_ABI hle_vsnprintf(char* buffer, SIZE_T count, const char* fmt, GuestVaList ap) {
  ArgReader a = crt::args_of(ap);
  return finish_sprintf(crt::kPrintfStandardSnprintfBehavior, buffer, count, crt::format_narrow(fmt, a, 0));
}
int32_t XBEM_GUEST_ABI hle__vsnprintf(char* buffer, SIZE_T count, const char* fmt, GuestVaList ap) {
  ArgReader a = crt::args_of(ap);
  return finish_sprintf(0, buffer, count, crt::format_narrow(fmt, a, 0));
}
int32_t XBEM_GUEST_ABI hle__vscprintf(const char* fmt, GuestVaList ap) {
  ArgReader a = crt::args_of(ap);
  return static_cast<int32_t>(crt::format_narrow(fmt, a, 0).size());
}

// --------------------------------------------------------------- FILE

void* XBEM_GUEST_ABI hle___acrt_iob_func(UINT index) { return crt::std_stream(index); }
void* XBEM_GUEST_ABI hle___iob_func() { return crt::std_stream(0); }

void* XBEM_GUEST_ABI hle_fopen(const char* path, const char* mode) {
  if (!path || !mode) {
    set_errno(crt::kEINVAL);
    return nullptr;
  }
  int err = 0;
  GuestFile* f = crt::open_guest_file(narrow(path), narrow(mode), &err);
  if (!f) set_errno(err);
  return f;
}

void* XBEM_GUEST_ABI hle__wfopen(const char16_t* path, const char16_t* mode) {
  if (!path || !mode) {
    set_errno(crt::kEINVAL);
    return nullptr;
  }
  int err = 0;
  GuestFile* f = crt::open_guest_file(narrow(path), narrow(mode), &err);
  if (!f) set_errno(err);
  return f;
}

int32_t XBEM_GUEST_ABI hle_fopen_s(void** out, const char* path, const char* mode) {
  if (!out) return crt::kEINVAL;
  *out = hle_fopen(path, mode);
  return *out ? 0 : *crt::errno_ptr();
}

int32_t XBEM_GUEST_ABI hle__wfopen_s(void** out, const char16_t* path, const char16_t* mode) {
  if (!out) return crt::kEINVAL;
  *out = hle__wfopen(path, mode);
  return *out ? 0 : *crt::errno_ptr();
}

void* XBEM_GUEST_ABI hle__fsopen(const char* path, const char* mode, int32_t) { return hle_fopen(path, mode); }
void* XBEM_GUEST_ABI hle__wfsopen(const char16_t* path, const char16_t* mode, int32_t) { return hle__wfopen(path, mode); }

int32_t XBEM_GUEST_ABI hle_fclose(void* stream) {
  GuestFile* f = crt::guest_file(stream);
  return f ? crt::close_guest_file(f) : -1;
}

SIZE_T XBEM_GUEST_ABI hle_fread(void* buffer, SIZE_T size, SIZE_T count, void* stream) {
  GuestFile* f = crt::guest_file(stream);
  if (!f || size == 0 || count == 0) return 0;
  return read_bytes(f, buffer, size * count) / size;
}

SIZE_T XBEM_GUEST_ABI hle_fwrite(const void* buffer, SIZE_T size, SIZE_T count, void* stream) {
  GuestFile* f = crt::guest_file(stream);
  if (!f || size == 0 || count == 0) return 0;
  return crt::write_guest_file(f, buffer, size * count) / size;
}

int32_t XBEM_GUEST_ABI hle_fgetc(void* stream) {
  GuestFile* f = crt::guest_file(stream);
  return f ? read_char(f) : EOF;
}

int32_t XBEM_GUEST_ABI hle_fputc(int32_t c, void* stream) {
  GuestFile* f = crt::guest_file(stream);
  const char ch = static_cast<char>(c);
  return f && crt::write_guest_file(f, &ch, 1) == 1 ? static_cast<uint8_t>(ch) : EOF;
}

char* XBEM_GUEST_ABI hle_fgets(char* buffer, int32_t n, void* stream) {
  GuestFile* f = crt::guest_file(stream);
  if (!f || !buffer || n <= 0) return nullptr;
  int32_t i = 0;
  while (i < n - 1) {
    const int c = read_char(f);
    if (c == EOF) break;
    buffer[i++] = static_cast<char>(c);
    if (c == '\n') break;
  }
  if (i == 0) return nullptr;
  buffer[i] = 0;
  return buffer;
}

int32_t XBEM_GUEST_ABI hle_fputs(const char* s, void* stream) {
  GuestFile* f = crt::guest_file(stream);
  if (!f || !s) return EOF;
  const size_t n = bounded_strlen(s);
  return crt::write_guest_file(f, s, n) == n ? 0 : EOF;
}

int32_t XBEM_GUEST_ABI hle_puts(const char* s) {
  if (hle_fputs(s, crt::std_stream(1)) == EOF) return EOF;
  return hle_fputc('\n', crt::std_stream(1)) == EOF ? EOF : 0;
}

int32_t XBEM_GUEST_ABI hle_putchar(int32_t c) { return hle_fputc(c, crt::std_stream(1)); }
int32_t XBEM_GUEST_ABI hle_getchar() { return hle_fgetc(crt::std_stream(0)); }

int32_t XBEM_GUEST_ABI hle_ungetc(int32_t c, void* stream) {
  GuestFile* f = crt::guest_file(stream);
  return f ? std::ungetc(c, f->host) : EOF;
}

int32_t XBEM_GUEST_ABI hle_fflush(void* stream) {
  if (!stream) return std::fflush(nullptr);
  GuestFile* f = crt::guest_file(stream);
  return f ? std::fflush(f->host) : EOF;
}

int32_t XBEM_GUEST_ABI hle__fseeki64(void* stream, int64_t offset, int32_t origin) {
  GuestFile* f = crt::guest_file(stream);
  if (!f) return -1;
#if defined(_WIN32)
  return _fseeki64(f->host, offset, origin);
#else
  return fseeko(f->host, static_cast<off_t>(offset), origin);
#endif
}

int32_t XBEM_GUEST_ABI hle_fseek(void* stream, int32_t offset, int32_t origin) { return hle__fseeki64(stream, offset, origin); }

int64_t XBEM_GUEST_ABI hle__ftelli64(void* stream) {
  GuestFile* f = crt::guest_file(stream);
  if (!f) return -1;
#if defined(_WIN32)
  return _ftelli64(f->host);
#else
  return static_cast<int64_t>(ftello(f->host));
#endif
}

int32_t XBEM_GUEST_ABI hle_ftell(void* stream) {
  const int64_t p = hle__ftelli64(stream);
  if (p > 0x7FFFFFFF) {
    set_errno(crt::kEINVAL);
    return -1;
  }
  return static_cast<int32_t>(p);
}

void XBEM_GUEST_ABI hle_rewind(void* stream) {
  if (GuestFile* f = crt::guest_file(stream)) std::rewind(f->host);
}

int32_t XBEM_GUEST_ABI hle_feof(void* stream) {
  GuestFile* f = crt::guest_file(stream);
  return f ? std::feof(f->host) : 0;
}

int32_t XBEM_GUEST_ABI hle_ferror(void* stream) {
  GuestFile* f = crt::guest_file(stream);
  return f ? std::ferror(f->host) : 0;
}

void XBEM_GUEST_ABI hle_clearerr(void* stream) {
  if (GuestFile* f = crt::guest_file(stream)) std::clearerr(f->host);
}

int32_t XBEM_GUEST_ABI hle_setvbuf(void* stream, char*, int32_t mode, SIZE_T size) {
  GuestFile* f = crt::guest_file(stream);
  if (!f) return -1;
  // MSVC: _IOFBF 0x0, _IOLBF 0x40, _IONBF 0x4. The guest buffer is not used.
  const int host_mode = mode == 0x4 ? _IONBF : mode == 0x40 ? _IOLBF : _IOFBF;
  return std::setvbuf(f->host, nullptr, host_mode, size ? size : BUFSIZ);
}

void XBEM_GUEST_ABI hle_setbuf(void* stream, char* buffer) { hle_setvbuf(stream, buffer, buffer ? 0 : 0x4, 512); }

int32_t XBEM_GUEST_ABI hle__fileno(void* stream) {
  GuestFile* f = crt::guest_file(stream);
  return f ? f->fd : -1;
}

int32_t XBEM_GUEST_ABI hle_fgetpos(void* stream, int64_t* pos) {
  if (!pos) return crt::kEINVAL;
  *pos = hle__ftelli64(stream);
  return *pos < 0 ? -1 : 0;
}

int32_t XBEM_GUEST_ABI hle_fsetpos(void* stream, const int64_t* pos) {
  return pos ? hle__fseeki64(stream, *pos, SEEK_SET) : crt::kEINVAL;
}

void XBEM_GUEST_ABI hle__lock_file(void* stream) {
  GuestFile* f = crt::guest_file(stream);
#if !defined(_WIN32)
  if (f) flockfile(f->host);
#else
  if (f) _lock_file(f->host);
#endif
}

void XBEM_GUEST_ABI hle__unlock_file(void* stream) {
  GuestFile* f = crt::guest_file(stream);
#if !defined(_WIN32)
  if (f) funlockfile(f->host);
#else
  if (f) _unlock_file(f->host);
#endif
}

int32_t XBEM_GUEST_ABI hle__get_stream_buffer_pointers(void* stream, char*** base, char*** ptr, int32_t** count) {
  GuestFile* f = crt::guest_file(stream);
  if (!f) return crt::kEINVAL;
  if (base) *base = &f->buffer_base;
  if (ptr) *ptr = &f->buffer_ptr;
  if (count) *count = &f->buffer_count;
  return 0;
}

// Wide characters on streams are stored as UTF-8.
uint32_t XBEM_GUEST_ABI hle_fputwc(uint32_t c, void* stream) {
  GuestFile* f = crt::guest_file(stream);
  if (!f) return 0xFFFF;
  const std::string s = crt::to_narrow(std::u16string(1, static_cast<char16_t>(c)));
  return crt::write_guest_file(f, s.data(), s.size()) == s.size() ? (c & 0xFFFF) : 0xFFFF;
}

uint32_t XBEM_GUEST_ABI hle_fgetwc(void* stream) {
  GuestFile* f = crt::guest_file(stream);
  if (!f) return 0xFFFF;
  const int c0 = read_char(f);
  if (c0 == EOF) return 0xFFFF;
  std::string bytes(1, static_cast<char>(c0));
  const int extra = (c0 & 0xE0) == 0xC0 ? 1 : (c0 & 0xF0) == 0xE0 ? 2 : (c0 & 0xF8) == 0xF0 ? 3 : 0;
  for (int i = 0; i < extra; ++i) {
    const int c = read_char(f);
    if (c == EOF) break;
    bytes.push_back(static_cast<char>(c));
  }
  const std::u16string w = crt::to_wide(bytes);
  return w.empty() ? 0xFFFF : w[0];
}

// wint_t ungetwc(wint_t c, FILE*): pushes back the UTF-8 encoding of c.
uint32_t XBEM_GUEST_ABI hle_ungetwc(uint32_t c, void* stream) {
  c &= 0xFFFF;
  if (c == 0xFFFF) return 0xFFFF;  // WEOF
  const std::string bytes = crt::to_narrow(std::u16string(1, static_cast<char16_t>(c)));
  for (auto it = bytes.rbegin(); it != bytes.rend(); ++it)
    if (hle_ungetc(static_cast<uint8_t>(*it), stream) == EOF) return 0xFFFF;
  return c;
}

int32_t XBEM_GUEST_ABI hle_fputws(const char16_t* s, void* stream) {
  GuestFile* f = crt::guest_file(stream);
  if (!f || !s) return EOF;
  const std::string n = crt::to_narrow(guest_wstr(s));
  return crt::write_guest_file(f, n.data(), n.size()) == n.size() ? 0 : EOF;
}

char16_t* XBEM_GUEST_ABI hle_fgetws(char16_t* buffer, int32_t n, void* stream) {
  if (!buffer || n <= 0) return nullptr;
  int32_t i = 0;
  while (i < n - 1) {
    const uint32_t c = hle_fgetwc(stream);
    if (c == 0xFFFF) break;
    buffer[i++] = static_cast<char16_t>(c);
    if (c == '\n') break;
  }
  if (i == 0) return nullptr;
  buffer[i] = 0;
  return buffer;
}

int32_t XBEM_GUEST_ABI hle__putws(const char16_t* s) {
  if (hle_fputws(s, crt::std_stream(1)) == EOF) return EOF;
  return hle_fputc('\n', crt::std_stream(1)) == EOF ? EOF : 0;
}

// ------------------------------------------------- low-level fd functions

int32_t open_fd(const std::string& path, int32_t oflag) {
  constexpr int32_t O_WRONLY = 0x1, O_RDWR = 0x2, O_APPEND = 0x8, O_CREAT = 0x100, O_TRUNC = 0x200, O_EXCL = 0x400,
                    O_BINARY = 0x8000;
  auto resolved = proc().vfs().resolve(std::string_view(path));
  std::error_code ec;
  const bool exists = resolved && std::filesystem::exists(resolved->host_path, ec);
  if ((oflag & O_CREAT) && (oflag & O_EXCL) && exists) {
    set_errno(crt::kEEXIST);
    return -1;
  }
  if (!exists && !(oflag & O_CREAT)) {
    set_errno(crt::kENOENT);
    return -1;
  }
  std::string mode;
  if (oflag & O_APPEND) mode = (oflag & O_RDWR) ? "a+" : "a";
  else if (oflag & O_RDWR) mode = (!exists || (oflag & O_TRUNC)) ? "w+" : "r+";
  else if (oflag & O_WRONLY) mode = (!exists || (oflag & O_TRUNC)) ? "w" : "r+";
  else mode = "r";
  if (oflag & O_BINARY) mode += 'b';
  int err = 0;
  GuestFile* f = crt::open_guest_file(path, mode, &err);
  if (!f) {
    set_errno(err);
    return -1;
  }
  return f->fd;
}

int32_t XBEM_GUEST_ABI hle__open(const char* path, int32_t oflag, ...) { return path ? open_fd(narrow(path), oflag) : -1; }
int32_t XBEM_GUEST_ABI hle__wopen(const char16_t* path, int32_t oflag, ...) { return path ? open_fd(narrow(path), oflag) : -1; }

int32_t XBEM_GUEST_ABI hle__sopen_s(int32_t* fd, const char* path, int32_t oflag, int32_t, int32_t) {
  if (!fd) return crt::kEINVAL;
  *fd = path ? open_fd(narrow(path), oflag) : -1;
  return *fd < 0 ? *crt::errno_ptr() : 0;
}

int32_t XBEM_GUEST_ABI hle__wsopen_s(int32_t* fd, const char16_t* path, int32_t oflag, int32_t, int32_t) {
  if (!fd) return crt::kEINVAL;
  *fd = path ? open_fd(narrow(path), oflag) : -1;
  return *fd < 0 ? *crt::errno_ptr() : 0;
}

int32_t XBEM_GUEST_ABI hle__close(int32_t fd) {
  GuestFile* f = crt::file_from_fd(fd);
  if (!f) {
    set_errno(crt::kEBADF);
    return -1;
  }
  return crt::close_guest_file(f);
}

int32_t XBEM_GUEST_ABI hle__read(int32_t fd, void* buffer, UINT count) {
  GuestFile* f = crt::file_from_fd(fd);
  if (!f) {
    set_errno(crt::kEBADF);
    return -1;
  }
  return static_cast<int32_t>(read_bytes(f, buffer, count));
}

int32_t XBEM_GUEST_ABI hle__write(int32_t fd, const void* buffer, UINT count) {
  GuestFile* f = crt::file_from_fd(fd);
  if (!f) {
    set_errno(crt::kEBADF);
    return -1;
  }
  const size_t n = crt::write_guest_file(f, buffer, count);
  if (fd <= 2) std::fflush(f->host);
  return static_cast<int32_t>(n);
}

int64_t XBEM_GUEST_ABI hle__lseeki64(int32_t fd, int64_t offset, int32_t origin) {
  GuestFile* f = crt::file_from_fd(fd);
  if (!f) {
    set_errno(crt::kEBADF);
    return -1;
  }
  if (hle__fseeki64(f, offset, origin) != 0) return -1;
  return hle__ftelli64(f);
}

int32_t XBEM_GUEST_ABI hle__lseek(int32_t fd, int32_t offset, int32_t origin) {
  return static_cast<int32_t>(hle__lseeki64(fd, offset, origin));
}

int64_t XBEM_GUEST_ABI hle__telli64(int32_t fd) { return hle__lseeki64(fd, 0, SEEK_CUR); }

int64_t XBEM_GUEST_ABI hle__filelengthi64(int32_t fd) {
  const int64_t cur = hle__lseeki64(fd, 0, SEEK_CUR);
  if (cur < 0) return -1;
  const int64_t end = hle__lseeki64(fd, 0, SEEK_END);
  hle__lseeki64(fd, cur, SEEK_SET);
  return end;
}

int32_t XBEM_GUEST_ABI hle__filelength(int32_t fd) { return static_cast<int32_t>(hle__filelengthi64(fd)); }

int32_t XBEM_GUEST_ABI hle__isatty(int32_t fd) {
#if defined(_WIN32)
  return fd >= 0 && fd <= 2 ? _isatty(fd) : 0;
#else
  return fd >= 0 && fd <= 2 ? isatty(fd) : 0;
#endif
}

int32_t XBEM_GUEST_ABI hle__setmode(int32_t fd, int32_t mode) {
  GuestFile* f = crt::file_from_fd(fd);
  if (!f) {
    set_errno(crt::kEBADF);
    return -1;
  }
  const int32_t old = (f->flags & crt::kFileText) ? 0x4000 : 0x8000;
  if (mode == 0x8000) f->flags &= ~crt::kFileText;
  else if (mode == 0x4000) f->flags |= crt::kFileText;
  return old;
}

int32_t XBEM_GUEST_ABI hle__commit(int32_t fd) {
  GuestFile* f = crt::file_from_fd(fd);
  return f ? std::fflush(f->host) : -1;
}

void* XBEM_GUEST_ABI hle__fdopen(int32_t fd, const char*) {
  GuestFile* f = crt::file_from_fd(fd);
  if (!f) set_errno(crt::kEBADF);
  return f;
}

int32_t XBEM_GUEST_ABI hle__fcloseall() { return 0; }

}  // namespace

void add_ucrt_stdio_exports(std::vector<ExportDef>& out) {
  out.insert(out.end(), {
      XBEM_HLE(__stdio_common_vsprintf), XBEM_HLE(__stdio_common_vsprintf_s), XBEM_HLE(__stdio_common_vsnprintf_s),
      XBEM_HLE(__stdio_common_vswprintf), XBEM_HLE(__stdio_common_vswprintf_s),
      XBEM_HLE(__stdio_common_vsnwprintf_s), XBEM_HLE(__stdio_common_vfprintf), XBEM_HLE(__stdio_common_vfwprintf),
      XBEM_HLE(__stdio_common_vsscanf), XBEM_HLE(__stdio_common_vswscanf),
      ExportDef{"__stdio_common_vfprintf_s", reinterpret_cast<void*>(&hle___stdio_common_vfprintf)},
      ExportDef{"__stdio_common_vfwprintf_s", reinterpret_cast<void*>(&hle___stdio_common_vfwprintf)},
      XBEM_HLE(printf), XBEM_HLE(fprintf), XBEM_HLE(sprintf), XBEM_HLE(snprintf), XBEM_HLE(_snprintf),
      XBEM_HLE(sscanf), XBEM_HLE(vprintf), XBEM_HLE(vfprintf), XBEM_HLE(vsprintf), XBEM_HLE(vsnprintf),
      XBEM_HLE(_vsnprintf), XBEM_HLE(_vscprintf), XBEM_HLE(__acrt_iob_func), XBEM_HLE(__iob_func), XBEM_HLE(fopen),
      XBEM_HLE(_wfopen), XBEM_HLE(fopen_s), XBEM_HLE(_wfopen_s), XBEM_HLE(_fsopen), XBEM_HLE(_wfsopen),
      XBEM_HLE(fclose), XBEM_HLE(fread), XBEM_HLE(fwrite), XBEM_HLE(fgetc), XBEM_HLE(fputc), XBEM_HLE(fgets),
      XBEM_HLE(fputs), XBEM_HLE(puts), XBEM_HLE(putchar), XBEM_HLE(getchar), XBEM_HLE(ungetc), XBEM_HLE(fflush),
      XBEM_HLE(_fseeki64), XBEM_HLE(fseek), XBEM_HLE(_ftelli64), XBEM_HLE(ftell), XBEM_HLE(rewind), XBEM_HLE(feof),
      XBEM_HLE(ferror), XBEM_HLE(clearerr), XBEM_HLE(setvbuf), XBEM_HLE(setbuf), XBEM_HLE(_fileno), XBEM_HLE(fgetpos),
      XBEM_HLE(fsetpos), XBEM_HLE(_lock_file), XBEM_HLE(_unlock_file), XBEM_HLE(_get_stream_buffer_pointers),
      XBEM_HLE(fputwc), XBEM_HLE(fgetwc), XBEM_HLE(fputws), XBEM_HLE(fgetws), XBEM_HLE(_putws), XBEM_HLE(_open),
      XBEM_HLE(_wopen), XBEM_HLE(_sopen_s), XBEM_HLE(_wsopen_s), XBEM_HLE(_close), XBEM_HLE(_read), XBEM_HLE(_write),
      XBEM_HLE(_lseeki64), XBEM_HLE(_lseek), XBEM_HLE(_telli64), XBEM_HLE(_filelengthi64), XBEM_HLE(_filelength),
      XBEM_HLE(_isatty), XBEM_HLE(_setmode), XBEM_HLE(_commit), XBEM_HLE(_fdopen), XBEM_HLE(_fcloseall),
      ExportDef{"getc", reinterpret_cast<void*>(&hle_fgetc)}, ExportDef{"putc", reinterpret_cast<void*>(&hle_fputc)},
      ExportDef{"_fgetc_nolock", reinterpret_cast<void*>(&hle_fgetc)},
      ExportDef{"_fputc_nolock", reinterpret_cast<void*>(&hle_fputc)},
      ExportDef{"_getc_nolock", reinterpret_cast<void*>(&hle_fgetc)},
      ExportDef{"_putc_nolock", reinterpret_cast<void*>(&hle_fputc)},
      ExportDef{"_fread_nolock", reinterpret_cast<void*>(&hle_fread)},
      ExportDef{"_fwrite_nolock", reinterpret_cast<void*>(&hle_fwrite)},
      ExportDef{"_fflush_nolock", reinterpret_cast<void*>(&hle_fflush)},
      ExportDef{"_fseeki64_nolock", reinterpret_cast<void*>(&hle__fseeki64)},
      ExportDef{"_ftelli64_nolock", reinterpret_cast<void*>(&hle__ftelli64)},
      ExportDef{"_fclose_nolock", reinterpret_cast<void*>(&hle_fclose)},
      ExportDef{"_ungetc_nolock", reinterpret_cast<void*>(&hle_ungetc)}, XBEM_HLE(ungetwc),
      ExportDef{"_ungetwc_nolock", reinterpret_cast<void*>(&hle_ungetwc)},
      ExportDef{"getwc", reinterpret_cast<void*>(&hle_fgetwc)}, ExportDef{"putwc", reinterpret_cast<void*>(&hle_fputwc)},
      ExportDef{"_tell", reinterpret_cast<void*>(&hle__telli64)},
  });
}

}  // namespace xbem::hle
