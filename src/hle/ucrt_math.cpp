// UCRT: <math.h>, floating-point classification helpers used by the MSVC
// <cmath> inlines (_dclass, _dtest, _dpcomp, ...), integer helpers and
// floating-point environment control (_controlfp, fenv) mapped onto MXCSR.

#include <cmath>
#include <cstdlib>
#include <xmmintrin.h>

#include "hle/ucrt.h"

namespace xbem::hle {
namespace {

using namespace win;

// Every function needs an ms_abi wrapper: besides the calling convention,
// Microsoft x64 also treats xmm6-xmm15 as callee-saved.
#define XBEM_MATH1(name)                                                        \
  double XBEM_GUEST_ABI hle_##name(double x) { return std::name(x); }           \
  float XBEM_GUEST_ABI hle_##name##f(float x) { return std::name(x); }
#define XBEM_MATH2(name)                                                                \
  double XBEM_GUEST_ABI hle_##name(double x, double y) { return std::name(x, y); }      \
  float XBEM_GUEST_ABI hle_##name##f(float x, float y) { return std::name(x, y); }

XBEM_MATH1(sin) XBEM_MATH1(cos) XBEM_MATH1(tan) XBEM_MATH1(asin) XBEM_MATH1(acos) XBEM_MATH1(atan)
XBEM_MATH1(sinh) XBEM_MATH1(cosh) XBEM_MATH1(tanh) XBEM_MATH1(asinh) XBEM_MATH1(acosh) XBEM_MATH1(atanh)
XBEM_MATH1(exp) XBEM_MATH1(exp2) XBEM_MATH1(expm1) XBEM_MATH1(log) XBEM_MATH1(log10) XBEM_MATH1(log2)
XBEM_MATH1(log1p) XBEM_MATH1(logb) XBEM_MATH1(sqrt) XBEM_MATH1(cbrt) XBEM_MATH1(ceil) XBEM_MATH1(floor)
XBEM_MATH1(trunc) XBEM_MATH1(round) XBEM_MATH1(rint) XBEM_MATH1(nearbyint) XBEM_MATH1(erf) XBEM_MATH1(erfc)
XBEM_MATH1(tgamma) XBEM_MATH1(lgamma) XBEM_MATH1(fabs)
XBEM_MATH2(atan2) XBEM_MATH2(pow) XBEM_MATH2(fmod) XBEM_MATH2(hypot) XBEM_MATH2(remainder) XBEM_MATH2(copysign)
XBEM_MATH2(fmin) XBEM_MATH2(fmax) XBEM_MATH2(fdim) XBEM_MATH2(nextafter)

double XBEM_GUEST_ABI hle_fma(double x, double y, double z) { return std::fma(x, y, z); }
float XBEM_GUEST_ABI hle_fmaf(float x, float y, float z) { return std::fma(x, y, z); }
double XBEM_GUEST_ABI hle_frexp(double x, int32_t* e) {
  int v = 0;
  const double r = std::frexp(x, &v);
  if (e) *e = v;
  return r;
}
float XBEM_GUEST_ABI hle_frexpf(float x, int32_t* e) {
  int v = 0;
  const float r = std::frexp(x, &v);
  if (e) *e = v;
  return r;
}
double XBEM_GUEST_ABI hle_ldexp(double x, int32_t e) { return std::ldexp(x, e); }
float XBEM_GUEST_ABI hle_ldexpf(float x, int32_t e) { return std::ldexp(x, e); }
double XBEM_GUEST_ABI hle_scalbn(double x, int32_t e) { return std::scalbn(x, e); }
float XBEM_GUEST_ABI hle_scalbnf(float x, int32_t e) { return std::scalbn(x, e); }
double XBEM_GUEST_ABI hle_scalbln(double x, int32_t e) { return std::scalbln(x, e); }
float XBEM_GUEST_ABI hle_scalblnf(float x, int32_t e) { return std::scalbln(x, e); }
double XBEM_GUEST_ABI hle_modf(double x, double* ip) {
  double i = 0;
  const double r = std::modf(x, &i);
  if (ip) *ip = i;
  return r;
}
float XBEM_GUEST_ABI hle_modff(float x, float* ip) {
  float i = 0;
  const float r = std::modf(x, &i);
  if (ip) *ip = i;
  return r;
}
double XBEM_GUEST_ABI hle_remquo(double x, double y, int32_t* q) {
  int v = 0;
  const double r = std::remquo(x, y, &v);
  if (q) *q = v;
  return r;
}
float XBEM_GUEST_ABI hle_remquof(float x, float y, int32_t* q) {
  int v = 0;
  const float r = std::remquo(x, y, &v);
  if (q) *q = v;
  return r;
}
int32_t XBEM_GUEST_ABI hle_ilogb(double x) { return std::ilogb(x); }
int32_t XBEM_GUEST_ABI hle_ilogbf(float x) { return std::ilogb(x); }
// Windows `long` is 32 bits.
int32_t XBEM_GUEST_ABI hle_lround(double x) { return static_cast<int32_t>(std::lround(x)); }
int32_t XBEM_GUEST_ABI hle_lroundf(float x) { return static_cast<int32_t>(std::lround(x)); }
int64_t XBEM_GUEST_ABI hle_llround(double x) { return std::llround(x); }
int64_t XBEM_GUEST_ABI hle_llroundf(float x) { return std::llround(x); }
int32_t XBEM_GUEST_ABI hle_lrint(double x) { return static_cast<int32_t>(std::lrint(x)); }
int32_t XBEM_GUEST_ABI hle_lrintf(float x) { return static_cast<int32_t>(std::lrint(x)); }
int64_t XBEM_GUEST_ABI hle_llrint(double x) { return std::llrint(x); }
int64_t XBEM_GUEST_ABI hle_llrintf(float x) { return std::llrint(x); }
double XBEM_GUEST_ABI hle_nan(const char*) { return std::nan(""); }
float XBEM_GUEST_ABI hle_nanf(const char*) { return std::nanf(""); }

double XBEM_GUEST_ABI hle__chgsign(double x) { return -x; }
float XBEM_GUEST_ABI hle__chgsignf(float x) { return -x; }
int32_t XBEM_GUEST_ABI hle__finite(double x) { return std::isfinite(x); }
int32_t XBEM_GUEST_ABI hle__finitef(float x) { return std::isfinite(x); }
int32_t XBEM_GUEST_ABI hle__isnan(double x) { return std::isnan(x); }
int32_t XBEM_GUEST_ABI hle__isnanf(float x) { return std::isnan(x); }

// <cmath> classification helpers: FP_INFINITE 1, FP_NAN 2, FP_NORMAL -1, FP_SUBNORMAL -2, FP_ZERO 0.
template <class T>
int16_t dclass(T x) {
  switch (std::fpclassify(x)) {
    case FP_INFINITE: return 1;
    case FP_NAN: return 2;
    case FP_SUBNORMAL: return -2;
    case FP_ZERO: return 0;
    default: return -1;
  }
}
int16_t XBEM_GUEST_ABI hle__dclass(double x) { return dclass(x); }
int16_t XBEM_GUEST_ABI hle__fdclass(float x) { return dclass(x); }
int16_t XBEM_GUEST_ABI hle__dtest(const double* x) { return dclass(*x); }
int16_t XBEM_GUEST_ABI hle__fdtest(const float* x) { return dclass(*x); }
int32_t XBEM_GUEST_ABI hle__dsign(double x) { return std::signbit(x) ? 0x8000 : 0; }
int32_t XBEM_GUEST_ABI hle__fdsign(float x) { return std::signbit(x) ? 0x8000 : 0; }

// _FP_LT 1, _FP_EQ 2, _FP_GT 4; 0 when unordered.
template <class T>
int32_t pcomp(T x, T y) {
  if (std::isnan(x) || std::isnan(y)) return 0;
  return x < y ? 1 : x == y ? 2 : 4;
}
int32_t XBEM_GUEST_ABI hle__dpcomp(double x, double y) { return pcomp(x, y); }
int32_t XBEM_GUEST_ABI hle__fdpcomp(float x, float y) { return pcomp(x, y); }

int32_t XBEM_GUEST_ABI hle__fpclass(double x) {
  const bool neg = std::signbit(x);
  switch (std::fpclassify(x)) {
    case FP_NAN: return 0x0002;  // _FPCLASS_QNAN
    case FP_INFINITE: return neg ? 0x0004 : 0x0200;
    case FP_SUBNORMAL: return neg ? 0x0010 : 0x0080;
    case FP_ZERO: return neg ? 0x0020 : 0x0040;
    default: return neg ? 0x0008 : 0x0100;
  }
}

int32_t g_fma3_enabled = 1;
int32_t XBEM_GUEST_ABI hle__set_FMA3_enable(int32_t v) { return std::exchange(g_fma3_enabled, v); }
int32_t XBEM_GUEST_ABI hle__get_FMA3_enable() { return g_fma3_enabled; }

// ----------------------------------------------------------- integers

int32_t XBEM_GUEST_ABI hle_abs(int32_t v) { return v < 0 ? -v : v; }
int32_t XBEM_GUEST_ABI hle_labs(int32_t v) { return v < 0 ? -v : v; }
int64_t XBEM_GUEST_ABI hle_llabs(int64_t v) { return v < 0 ? -v : v; }

struct DivResult {
  int32_t quot, rem;
};
struct LLDivResult {
  int64_t quot, rem;
};
DivResult XBEM_GUEST_ABI hle_div(int32_t n, int32_t d) { return {n / d, n % d}; }
LLDivResult XBEM_GUEST_ABI hle_lldiv(int64_t n, int64_t d) { return {n / d, n % d}; }

// ------------------------------------------------ floating-point control

uint32_t mxcsr_to_control(uint32_t m) {
  uint32_t cw = 0;
  if (m & 0x1000) cw |= 0x01;     // _EM_INEXACT
  if (m & 0x0800) cw |= 0x02;     // _EM_UNDERFLOW
  if (m & 0x0400) cw |= 0x04;     // _EM_OVERFLOW
  if (m & 0x0200) cw |= 0x08;     // _EM_ZERODIVIDE
  if (m & 0x0080) cw |= 0x10;     // _EM_INVALID
  if (m & 0x0100) cw |= 0x80000;  // _EM_DENORMAL
  cw |= ((m >> 13) & 3) << 8;     // _MCW_RC: 0 near, 0x100 down, 0x200 up, 0x300 chop
  const bool fz = m & 0x8000, daz = m & 0x40;
  if (fz && daz) cw |= 0x01000000;       // _DN_FLUSH
  else if (daz) cw |= 0x02000000;        // flush operands, save results
  else if (fz) cw |= 0x03000000;         // save operands, flush results
  return cw;
}

uint32_t control_to_mxcsr(uint32_t cw, uint32_t m) {
  m &= ~(0x1F80u | 0x6000u | 0x8040u);
  if (cw & 0x01) m |= 0x1000;
  if (cw & 0x02) m |= 0x0800;
  if (cw & 0x04) m |= 0x0400;
  if (cw & 0x08) m |= 0x0200;
  if (cw & 0x10) m |= 0x0080;
  if (cw & 0x80000) m |= 0x0100;
  m |= ((cw >> 8) & 3) << 13;
  switch (cw & 0x03000000) {
    case 0x01000000: m |= 0x8040; break;
    case 0x02000000: m |= 0x0040; break;
    case 0x03000000: m |= 0x8000; break;
  }
  return m;
}

uint32_t mxcsr_to_status(uint32_t m) {
  uint32_t sw = 0;
  if (m & 0x20) sw |= 0x01;     // inexact
  if (m & 0x10) sw |= 0x02;     // underflow
  if (m & 0x08) sw |= 0x04;     // overflow
  if (m & 0x04) sw |= 0x08;     // zero divide
  if (m & 0x01) sw |= 0x10;     // invalid
  if (m & 0x02) sw |= 0x80000;  // denormal
  return sw;
}

uint32_t status_to_mxcsr_flags(uint32_t sw) {
  uint32_t f = 0;
  if (sw & 0x01) f |= 0x20;
  if (sw & 0x02) f |= 0x10;
  if (sw & 0x04) f |= 0x08;
  if (sw & 0x08) f |= 0x04;
  if (sw & 0x10) f |= 0x01;
  if (sw & 0x80000) f |= 0x02;
  return f;
}

uint32_t XBEM_GUEST_ABI hle__controlfp(uint32_t value, uint32_t mask) {
  const uint32_t m = _mm_getcsr();
  uint32_t cw = mxcsr_to_control(m);
  mask &= 0x0308031F;  // _MCW_EM | _MCW_RC | _MCW_DN (precision/infinity control do not exist on x64)
  cw = (cw & ~mask) | (value & mask);
  _mm_setcsr(control_to_mxcsr(cw, m));
  return cw;
}

int32_t XBEM_GUEST_ABI hle__controlfp_s(uint32_t* current, uint32_t value, uint32_t mask) {
  const uint32_t cw = mask ? hle__controlfp(value, mask) : mxcsr_to_control(_mm_getcsr());
  if (current) *current = cw;
  return 0;
}

uint32_t XBEM_GUEST_ABI hle__control87(uint32_t value, uint32_t mask) { return hle__controlfp(value, mask); }
uint32_t XBEM_GUEST_ABI hle__statusfp() { return mxcsr_to_status(_mm_getcsr()); }
uint32_t XBEM_GUEST_ABI hle__clearfp() {
  const uint32_t m = _mm_getcsr();
  _mm_setcsr(m & ~0x3Fu);
  return mxcsr_to_status(m);
}
void XBEM_GUEST_ABI hle__fpreset() { _mm_setcsr(0x1F80); }

struct Fenv {
  uint32_t control;
  uint32_t status;
};

int32_t XBEM_GUEST_ABI hle_fegetenv(Fenv* env) {
  if (!env) return 1;
  const uint32_t m = _mm_getcsr();
  env->control = mxcsr_to_control(m);
  env->status = mxcsr_to_status(m);
  return 0;
}
int32_t XBEM_GUEST_ABI hle_fesetenv(const Fenv* env) {
  if (!env) return 1;
  const uint32_t m = control_to_mxcsr(env->control, _mm_getcsr()) & ~0x3Fu;
  _mm_setcsr(m | status_to_mxcsr_flags(env->status));
  return 0;
}
int32_t XBEM_GUEST_ABI hle_feholdexcept(Fenv* env) {
  hle_fegetenv(env);
  _mm_setcsr((_mm_getcsr() & ~0x3Fu) | 0x1F80u);  // clear flags, mask everything
  return 0;
}
int32_t XBEM_GUEST_ABI hle_feupdateenv(const Fenv* env) {
  const uint32_t raised = _mm_getcsr() & 0x3F;
  hle_fesetenv(env);
  _mm_setcsr(_mm_getcsr() | raised);
  return 0;
}
int32_t XBEM_GUEST_ABI hle_fegetround() { return static_cast<int32_t>(((_mm_getcsr() >> 13) & 3) << 8); }
int32_t XBEM_GUEST_ABI hle_fesetround(int32_t r) {
  if (r & ~0x300) return 1;
  _mm_setcsr((_mm_getcsr() & ~0x6000u) | ((static_cast<uint32_t>(r) >> 8) << 13));
  return 0;
}
int32_t XBEM_GUEST_ABI hle_feclearexcept(int32_t e) {
  _mm_setcsr(_mm_getcsr() & ~status_to_mxcsr_flags(static_cast<uint32_t>(e)));
  return 0;
}
int32_t XBEM_GUEST_ABI hle_fetestexcept(int32_t e) {
  return static_cast<int32_t>(mxcsr_to_status(_mm_getcsr()) & static_cast<uint32_t>(e) & 0x1F);
}
int32_t XBEM_GUEST_ABI hle_feraiseexcept(int32_t e) {
  _mm_setcsr(_mm_getcsr() | status_to_mxcsr_flags(static_cast<uint32_t>(e)));
  return 0;
}
int32_t XBEM_GUEST_ABI hle_fegetexceptflag(uint32_t* flags, int32_t e) {
  if (!flags) return 1;
  *flags = mxcsr_to_status(_mm_getcsr()) & static_cast<uint32_t>(e);
  return 0;
}
int32_t XBEM_GUEST_ABI hle_fesetexceptflag(const uint32_t* flags, int32_t e) {
  if (!flags) return 1;
  const uint32_t mask = status_to_mxcsr_flags(static_cast<uint32_t>(e));
  _mm_setcsr((_mm_getcsr() & ~mask) | (status_to_mxcsr_flags(*flags) & mask));
  return 0;
}

#define XBEM_MATH_EXPORTS(name) XBEM_HLE(name), XBEM_HLE(name##f)

}  // namespace

void add_ucrt_math_exports(std::vector<ExportDef>& out) {
  out.insert(out.end(), {
      XBEM_MATH_EXPORTS(sin), XBEM_MATH_EXPORTS(cos), XBEM_MATH_EXPORTS(tan), XBEM_MATH_EXPORTS(asin),
      XBEM_MATH_EXPORTS(acos), XBEM_MATH_EXPORTS(atan), XBEM_MATH_EXPORTS(sinh), XBEM_MATH_EXPORTS(cosh),
      XBEM_MATH_EXPORTS(tanh), XBEM_MATH_EXPORTS(asinh), XBEM_MATH_EXPORTS(acosh), XBEM_MATH_EXPORTS(atanh),
      XBEM_MATH_EXPORTS(exp), XBEM_MATH_EXPORTS(exp2), XBEM_MATH_EXPORTS(expm1), XBEM_MATH_EXPORTS(log),
      XBEM_MATH_EXPORTS(log10), XBEM_MATH_EXPORTS(log2), XBEM_MATH_EXPORTS(log1p), XBEM_MATH_EXPORTS(logb),
      XBEM_MATH_EXPORTS(sqrt), XBEM_MATH_EXPORTS(cbrt), XBEM_MATH_EXPORTS(ceil), XBEM_MATH_EXPORTS(floor),
      XBEM_MATH_EXPORTS(trunc), XBEM_MATH_EXPORTS(round), XBEM_MATH_EXPORTS(rint), XBEM_MATH_EXPORTS(nearbyint),
      XBEM_MATH_EXPORTS(erf), XBEM_MATH_EXPORTS(erfc), XBEM_MATH_EXPORTS(tgamma), XBEM_MATH_EXPORTS(lgamma),
      XBEM_MATH_EXPORTS(fabs), XBEM_MATH_EXPORTS(atan2), XBEM_MATH_EXPORTS(pow), XBEM_MATH_EXPORTS(fmod),
      XBEM_MATH_EXPORTS(hypot), XBEM_MATH_EXPORTS(remainder), XBEM_MATH_EXPORTS(copysign), XBEM_MATH_EXPORTS(fmin),
      XBEM_MATH_EXPORTS(fmax), XBEM_MATH_EXPORTS(fdim), XBEM_MATH_EXPORTS(nextafter), XBEM_MATH_EXPORTS(fma),
      XBEM_MATH_EXPORTS(frexp), XBEM_MATH_EXPORTS(ldexp), XBEM_MATH_EXPORTS(scalbn), XBEM_MATH_EXPORTS(scalbln),
      XBEM_MATH_EXPORTS(modf), XBEM_MATH_EXPORTS(remquo), XBEM_MATH_EXPORTS(ilogb), XBEM_MATH_EXPORTS(lround),
      XBEM_MATH_EXPORTS(llround), XBEM_MATH_EXPORTS(lrint), XBEM_MATH_EXPORTS(llrint), XBEM_MATH_EXPORTS(nan),
      ExportDef{"_hypot", reinterpret_cast<void*>(&hle_hypot)}, ExportDef{"_hypotf", reinterpret_cast<void*>(&hle_hypotf)},
      ExportDef{"_copysign", reinterpret_cast<void*>(&hle_copysign)},
      ExportDef{"_copysignf", reinterpret_cast<void*>(&hle_copysignf)},
      ExportDef{"_logb", reinterpret_cast<void*>(&hle_logb)}, ExportDef{"_logbf", reinterpret_cast<void*>(&hle_logbf)},
      ExportDef{"_nextafter", reinterpret_cast<void*>(&hle_nextafter)},
      ExportDef{"_nextafterf", reinterpret_cast<void*>(&hle_nextafterf)},
      ExportDef{"_scalb", reinterpret_cast<void*>(&hle_scalbln)},
      ExportDef{"_scalbf", reinterpret_cast<void*>(&hle_scalblnf)}, XBEM_HLE(_chgsign), XBEM_HLE(_chgsignf),
      XBEM_HLE(_finite), XBEM_HLE(_finitef), XBEM_HLE(_isnan), XBEM_HLE(_isnanf), XBEM_HLE(_dclass),
      XBEM_HLE(_fdclass), XBEM_HLE(_dtest), XBEM_HLE(_fdtest), XBEM_HLE(_dsign), XBEM_HLE(_fdsign),
      XBEM_HLE(_dpcomp), XBEM_HLE(_fdpcomp), XBEM_HLE(_fpclass), XBEM_HLE(_set_FMA3_enable),
      XBEM_HLE(_get_FMA3_enable),
      // long double is double on Windows x64.
      ExportDef{"_ldclass", reinterpret_cast<void*>(&hle__dclass)},
      ExportDef{"_ldtest", reinterpret_cast<void*>(&hle__dtest)},
      ExportDef{"_ldsign", reinterpret_cast<void*>(&hle__dsign)},
      ExportDef{"_ldpcomp", reinterpret_cast<void*>(&hle__dpcomp)}, XBEM_HLE(abs), XBEM_HLE(labs), XBEM_HLE(llabs),
      ExportDef{"_abs64", reinterpret_cast<void*>(&hle_llabs)},
      ExportDef{"imaxabs", reinterpret_cast<void*>(&hle_llabs)}, XBEM_HLE(div),
      ExportDef{"ldiv", reinterpret_cast<void*>(&hle_div)}, XBEM_HLE(lldiv),
      ExportDef{"imaxdiv", reinterpret_cast<void*>(&hle_lldiv)}, XBEM_HLE(_controlfp), XBEM_HLE(_controlfp_s),
      XBEM_HLE(_control87), XBEM_HLE(_statusfp), XBEM_HLE(_clearfp), XBEM_HLE(_fpreset), XBEM_HLE(fegetenv),
      XBEM_HLE(fesetenv), XBEM_HLE(feholdexcept), XBEM_HLE(feupdateenv), XBEM_HLE(fegetround), XBEM_HLE(fesetround),
      XBEM_HLE(feclearexcept), XBEM_HLE(fetestexcept), XBEM_HLE(feraiseexcept), XBEM_HLE(fegetexceptflag),
      XBEM_HLE(fesetexceptflag),
  });
}

}  // namespace xbem::hle
