#pragma once

#include <cstdint>

namespace core
{

[[nodiscard]] uint32_t RoundUp(uint32_t numToRound, uint32_t multiple);

// the math functions every platform computes alike: LLVM libc's, correctly rounded, the one right answer, which any two
// platforms agree on (see core/math.cpp). the platforms' own (std::sin and the rest, Apple's, glibc's and Windows' libm)
// each round some inputs their own way: Apple's double tan 41% of them, its sin and cos 4%, acos 15%. call these, never
// std::'s, nor glm's functions that call std::'s (core/glm_extra.h has those: Perspective, Rotation, AngleAxis, Slerp).
// sqrt, fma, floor, fmod and the rest IEEE 754 rounds exactly, anywhere: std::'s
[[nodiscard]] float Sin(float value) noexcept;
[[nodiscard]] double Sin(double value) noexcept;
[[nodiscard]] float Cos(float value) noexcept;
[[nodiscard]] double Cos(double value) noexcept;
[[nodiscard]] float Tan(float value) noexcept;
[[nodiscard]] double Tan(double value) noexcept;
[[nodiscard]] float Asin(float value) noexcept;
[[nodiscard]] double Asin(double value) noexcept;
[[nodiscard]] float Acos(float value) noexcept;
[[nodiscard]] double Acos(double value) noexcept;
[[nodiscard]] float Atan(float value) noexcept;
[[nodiscard]] double Atan(double value) noexcept;
[[nodiscard]] float Atan2(float ordinate, float abscissa) noexcept;
[[nodiscard]] double Atan2(double ordinate, double abscissa) noexcept;
[[nodiscard]] float Exp(float value) noexcept;
[[nodiscard]] double Exp(double value) noexcept;
[[nodiscard]] float Exp2(float value) noexcept;
[[nodiscard]] double Exp2(double value) noexcept;
[[nodiscard]] float Log2(float value) noexcept;
[[nodiscard]] double Log2(double value) noexcept;
[[nodiscard]] float Log10(float value) noexcept;
[[nodiscard]] double Log10(double value) noexcept;
[[nodiscard]] float Pow(float base, float exponent) noexcept;
[[nodiscard]] double Pow(double base, double exponent) noexcept;
[[nodiscard]] float Cbrt(float value) noexcept;
[[nodiscard]] double Cbrt(double value) noexcept;

} // namespace core
