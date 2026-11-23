#include "hle/discovery.h"

#include <format>

#include "common/log.h"
#include "common/strings.h"
#include "platform/memory.h"
#include "runtime/process.h"

namespace xbem::hle {

namespace {

std::string ascii_at(const uint8_t* bytes, size_t n) {
  std::string text;
  for (size_t i = 0; i < n; ++i) {
    const uint8_t b = bytes[i];
    if (b == 0) break;
    if (b < 0x20 || b > 0x7E) return {};
    text.push_back(static_cast<char>(b));
  }
  return text;
}

std::string utf16_at(const uint8_t* bytes, size_t n) {
  std::string text;
  for (size_t i = 0; i + 1 < n; i += 2) {
    const uint16_t c = static_cast<uint16_t>(bytes[i] | (bytes[i + 1] << 8));
    if (c == 0) break;
    if (c < 0x20 || c > 0x7E) return {};
    text.push_back(static_cast<char>(c));
  }
  return text;
}

}  // namespace

std::string describe_arg(uint64_t v) {
  std::string out = std::format("{:#x}", v);
  uint8_t bytes[64];
  if (v < 0x10000 || v >= 0x800000000000ull || !platform::safe_read(v, bytes, 16)) return out;
  out += " -> {" + guid_to_string(bytes) + "}";
  if (platform::safe_read(v, bytes, sizeof bytes)) {
    if (std::string a = ascii_at(bytes, sizeof bytes); a.size() >= 3) out += " \"" + a + "\"";
    else if (std::string w = utf16_at(bytes, sizeof bytes); w.size() >= 3) out += " L\"" + w + "\"";
  }
  return out;
}

std::string describe_code_address(uint64_t address) {
  char buf[256] = {0};
  if (rt::Process* p = rt::Process::current_or_null(); p && p->loader().describe(address, buf, sizeof buf))
    return buf;
  return std::format("{:#x}", address);
}

int32_t success_value_for_caller(uint64_t ret) {
  uint8_t code[32];
  if (!platform::safe_read(ret, code, sizeof code)) return 1;
  for (size_t i = 0; i + 3 < sizeof code; ++i) {
    // test r32,r32 with identical registers (85 /r, mod=11, reg==rm), optionally REX-prefixed.
    size_t j = i;
    if ((code[j] & 0xF0) == 0x40) ++j;
    if (j + 3 >= sizeof code || code[j] != 0x85) continue;
    const uint8_t modrm = code[j + 1];
    if ((modrm & 0xC0) != 0xC0 || ((modrm >> 3) & 7) != (modrm & 7)) continue;
    const uint8_t next = code[j + 2];
    if (next == 0x78 || next == 0x79) return 0;                                    // js / jns rel8
    if (next == 0x0F && (code[j + 3] == 0x88 || code[j + 3] == 0x89)) return 0;  // js / jns rel32
    return 1;  // first value test is not a sign test: BOOL-style
  }
  return 1;
}

int64_t failure_value_for_caller(uint64_t return_address) {
  return success_value_for_caller(return_address) == 0 ? static_cast<int32_t>(0x80004001) : 0;
}

void log_api_call(const char* category, const char* function, std::initializer_list<uint64_t> args,
                  uint64_t return_address, std::string_view outcome) {
  std::string_view name = function;
  if (name.starts_with("hle_")) name.remove_prefix(4);
  std::string text;
  for (uint64_t a : args) {
    if (!text.empty()) text += ", ";
    text += describe_arg(a);
  }
  XBEM_WARN(category, "{}({}) from {} -> {}", name, text, describe_code_address(return_address), outcome);
}

}  // namespace xbem::hle
