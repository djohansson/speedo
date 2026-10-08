#pragma once

#include <gfx/environment.h>

#include <cstddef>
#include <vector>

// the environment's prefiltering kernels (src/gfx/shaders/environment.slang) run on the cpu, as slang's c++ target
// compiles them (environmentkernels.generated.cpp, a build step of assettest): the same kernels the client's
// gfx::EnvironmentFilter runs on the gpu, in the same order, so the tests check what the client draws with.
namespace environmentkernels
{

// what to prefilter: a file's panoramas (the original, dominant lights included, and the lighting one without them, see
// gfx::environment::ExtractDominantLights), or the procedural sky's parameters, at its source width
struct Source
{
	const gfx::environment::Panorama* original = nullptr;
	const gfx::environment::Panorama* lighting = nullptr;
	const gfx::environment::SkyParameters* sky = nullptr;
	uint32_t skyWidth = gfx::environment::kSkyWidth;
};

// the levels and sheen levels (as R16G16B16A16 half floats in data, laid out as gfx::environment::Environment says) and
// the irradiance (dominantLights is left empty: the caller has them)
[[nodiscard]] gfx::environment::Environment Prefilter(const Source& source, std::vector<std::byte>& data);

} // namespace environmentkernels
