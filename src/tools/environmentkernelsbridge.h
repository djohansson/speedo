#pragma once

#include <gfx/shaders/capi.h>

#include <core/std_extra.h>

#include <array>
#include <cstdint>
#include <vector>

// environment.slang's kernels run on the cpu (see environmentkernels.h), behind an interface of plain types: slang's
// prelude, which the generated code and this side include, declares many names globally (half, Vector, uint3, ...),
// kept away from the gfx and glm headers of the other side.
namespace environmentkernels::bridge
{

// float rgba texels, row by row
struct Level
{
	uint32_t width = 0;
	uint32_t height = 0;
	std::vector<std::array<float, 4>> texels;
};

// a file's panoramas (the original and the lighting one, rgb floats row by row), or the sky (for the sun in environment's
// sky fields, its panorama sourceWidth wide)
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
	// the sky's: its panorama (the source's level 0, without its sun), and its transmittance table (see sky.h)
	Level skyPanorama;
	Level skyTransmittance;
};

// calls body(index) for each index below count, on several threads, and returns once all are done (the other side's
// core::TaskExecutor::ForEach)
using ForEach = core::std_extra::function_ref<void(uint32_t count, core::std_extra::function_ref<void(uint32_t index)> body)>;

// runs the kernels as gfx::EnvironmentFilter does on the gpu: levelCount levels, and the irradiance into environment, each
// kernel's rows of groups spread by forEach
[[nodiscard]] Output Prefilter(const Input& input, uint32_t levelCount, EnvironmentData& environment, ForEach forEach);

} // namespace environmentkernels::bridge
