#pragma once

#include <gfx/shaders/capi.h>

#include <array>
#include <cstdint>
#include <vector>

// environment.slang's kernels run on the cpu (see environmentkernels.h), behind an interface of plain types: the
// generated code and slang's prelude compile as c++20 (the prelude wants <stdfloat> from c++23 on, which libc++ lacks),
// which the gfx headers don't.
namespace environmentkernels::bridge
{

// float rgba texels, row by row
struct Level
{
	uint32_t width = 0;
	uint32_t height = 0;
	std::vector<std::array<float, 4>> texels;
};

// a file's panoramas (the original and the lighting one, rgb floats row by row), or the procedural sky (environment's
// sky fields, then sourceWidth wide)
struct Input
{
	uint32_t width = 0;
	uint32_t height = 0;
	const float* original = nullptr;
	const float* lighting = nullptr;
	uint32_t sourceWidth = 0;
};

struct Output
{
	std::vector<Level> levels; // kLevelCount, from the source's width
	std::vector<Level> sheenLevels; // kLevelCount, from an eighth of it
};

// runs the kernels as gfx::EnvironmentFilter does on the gpu: levelCount levels, and the irradiance into environment
[[nodiscard]] Output Prefilter(const Input& input, uint32_t levelCount, EnvironmentData& environment);

} // namespace environmentkernels::bridge
