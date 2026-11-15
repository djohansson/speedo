#include <core/math.h>//NOLINT(modernize-deprecated-headers)

// LLVM libc's math functions (the llvm-libc-math port: libc/shared/math, header only), each function's header alone (the
// whole of shared/math.h is 1086 headers). configured as the toolchain's flags have it: no errno, no floating point
// exception flags (nothing reads them), round to nearest only (nothing sets another mode; it saves the query each call).
// not LIBC_MATH_SKIP_ACCURATE_PASS, which is what rounds them correctly
#define LIBC_MATH (LIBC_MATH_NO_ERRNO | LIBC_MATH_NO_EXCEPT | LIBC_MATH_ASSUME_ROUND_NEAREST_ONLY)
#include <shared/math/acos.h>
#include <shared/math/acosf.h>
#include <shared/math/asin.h>
#include <shared/math/asinf.h>
#include <shared/math/atan.h>
#include <shared/math/atan2.h>
#include <shared/math/atan2f.h>
#include <shared/math/atanf.h>
#include <shared/math/cbrt.h>
#include <shared/math/cbrtf.h>
#include <shared/math/cos.h>
#include <shared/math/cosf.h>
#include <shared/math/exp.h>
#include <shared/math/exp2.h>
#include <shared/math/exp2f.h>
#include <shared/math/expf.h>
#include <shared/math/log10.h>
#include <shared/math/log10f.h>
#include <shared/math/log2.h>
#include <shared/math/log2f.h>
#include <shared/math/pow.h>
#include <shared/math/powf.h>
#include <shared/math/sin.h>
#include <shared/math/sinf.h>
#include <shared/math/tan.h>
#include <shared/math/tanf.h>

namespace core
{

uint32_t RoundUp(uint32_t numToRound, uint32_t multiple)
{
	if (multiple == 0)
		return numToRound;

	auto remainder = numToRound % multiple;
	if (remainder == 0)
		return numToRound;

	return numToRound + multiple - remainder;
}

namespace libc = LIBC_NAMESPACE::shared;

float Sin(float value) noexcept { return libc::sinf(value); }
double Sin(double value) noexcept { return libc::sin(value); }
float Cos(float value) noexcept { return libc::cosf(value); }
double Cos(double value) noexcept { return libc::cos(value); }
float Tan(float value) noexcept { return libc::tanf(value); }
double Tan(double value) noexcept { return libc::tan(value); }
float Asin(float value) noexcept { return libc::asinf(value); }
double Asin(double value) noexcept { return libc::asin(value); }
float Acos(float value) noexcept { return libc::acosf(value); }
double Acos(double value) noexcept { return libc::acos(value); }
float Atan(float value) noexcept { return libc::atanf(value); }
double Atan(double value) noexcept { return libc::atan(value); }
float Atan2(float ordinate, float abscissa) noexcept { return libc::atan2f(ordinate, abscissa); }
double Atan2(double ordinate, double abscissa) noexcept { return libc::atan2(ordinate, abscissa); }
float Exp(float value) noexcept { return libc::expf(value); }
double Exp(double value) noexcept { return libc::exp(value); }
float Exp2(float value) noexcept { return libc::exp2f(value); }
double Exp2(double value) noexcept { return libc::exp2(value); }
float Log2(float value) noexcept { return libc::log2f(value); }
double Log2(double value) noexcept { return libc::log2(value); }
float Log10(float value) noexcept { return libc::log10f(value); }
double Log10(double value) noexcept { return libc::log10(value); }
float Pow(float base, float exponent) noexcept { return libc::powf(base, exponent); }
double Pow(double base, double exponent) noexcept { return libc::pow(base, exponent); }
float Cbrt(float value) noexcept { return libc::cbrtf(value); }
double Cbrt(double value) noexcept { return libc::cbrt(value); }

} // namespace core
