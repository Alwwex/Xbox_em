#include "platform/cpu.h"

#include <cstdint>
#include <cstring>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

namespace xbem::platform {

namespace {

struct Regs {
  uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
};

Regs cpuid(uint32_t leaf, uint32_t subleaf = 0) {
  Regs r;
#if defined(_MSC_VER)
  int out[4];
  __cpuidex(out, static_cast<int>(leaf), static_cast<int>(subleaf));
  r.eax = static_cast<uint32_t>(out[0]);
  r.ebx = static_cast<uint32_t>(out[1]);
  r.ecx = static_cast<uint32_t>(out[2]);
  r.edx = static_cast<uint32_t>(out[3]);
#elif defined(__x86_64__) || defined(__i386__)
  __cpuid_count(leaf, subleaf, r.eax, r.ebx, r.ecx, r.edx);
#else
  (void)leaf;
  (void)subleaf;
#endif
  return r;
}

uint64_t xgetbv0() {
#if defined(_MSC_VER)
  return _xgetbv(0);
#elif defined(__x86_64__) || defined(__i386__)
  uint32_t lo, hi;
  __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
  return (static_cast<uint64_t>(hi) << 32) | lo;
#else
  return 0;
#endif
}

bool bit(uint32_t v, int b) { return (v >> b) & 1u; }

}  // namespace

std::string host_cpu_brand() {
  const uint32_t max_ext = cpuid(0x80000000).eax;
  if (max_ext < 0x80000004) return "unknown";
  char brand[49] = {};
  for (uint32_t i = 0; i < 3; ++i) {
    const Regs r = cpuid(0x80000002 + i);
    std::memcpy(brand + i * 16 + 0, &r.eax, 4);
    std::memcpy(brand + i * 16 + 4, &r.ebx, 4);
    std::memcpy(brand + i * 16 + 8, &r.ecx, 4);
    std::memcpy(brand + i * 16 + 12, &r.edx, 4);
  }
  std::string s(brand);
  const size_t first = s.find_first_not_of(' ');
  return first == std::string::npos ? s : s.substr(first);
}

std::vector<CpuFeature> host_cpu_features() {
  const uint32_t max_leaf = cpuid(0).eax;
  const Regs l1 = cpuid(1);
  const Regs l7 = max_leaf >= 7 ? cpuid(7, 0) : Regs{};
  const Regs e1 = cpuid(0x80000000).eax >= 0x80000001 ? cpuid(0x80000001) : Regs{};

  const bool osxsave = bit(l1.ecx, 27);
  const bool os_avx = osxsave && (xgetbv0() & 0x6) == 0x6;

  return {
      {"SSE3", bit(l1.ecx, 0), true},
      {"SSSE3", bit(l1.ecx, 9), true},
      {"SSE4.1", bit(l1.ecx, 19), true},
      {"SSE4.2", bit(l1.ecx, 20), true},
      {"POPCNT", bit(l1.ecx, 23), true},
      {"CMPXCHG16B", bit(l1.ecx, 13), true},
      {"PCLMULQDQ", bit(l1.ecx, 1), true},
      {"AES-NI", bit(l1.ecx, 25), true},
      {"AVX", bit(l1.ecx, 28) && os_avx, true},
      {"AVX2", bit(l7.ebx, 5) && os_avx, true},
      {"FMA3", bit(l1.ecx, 12) && os_avx, true},
      {"F16C", bit(l1.ecx, 29) && os_avx, true},
      {"BMI1", bit(l7.ebx, 3), true},
      {"BMI2", bit(l7.ebx, 8), true},
      {"LZCNT", bit(e1.ecx, 5), true},
      {"MOVBE", bit(l1.ecx, 22), true},
      {"SHA", bit(l7.ebx, 29), true},
      {"RDRAND", bit(l1.ecx, 30), false},
      {"RDSEED", bit(l7.ebx, 18), false},
      {"ADX", bit(l7.ebx, 19), false},
      {"RDPID", bit(l7.ecx, 22), false},
  };
}

bool host_has_feature(const char* name) {
  for (const CpuFeature& f : host_cpu_features())
    if (std::strcmp(f.name, name) == 0) return f.present;
  return false;
}

}  // namespace xbem::platform
