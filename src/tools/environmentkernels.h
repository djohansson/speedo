#pragma once

#include <gfx/environment.h>

#include <glm/vec3.hpp>

#include <cstddef>
#include <optional>
#include <vector>

namespace core
{
class TaskExecutor;
} // namespace core

// the environment's prefiltering kernels (src/gfx/shaders/environment.slang) run on the cpu, as slang's c++ target
// compiles them (environmentkernels.generated.cpp, a build step of assettest): the same kernels the client's
// gfx::EnvironmentFilter runs on the gpu, in the same order, so the tests check what the client draws with.
namespace environmentkernels
{

// what to prefilter: a file's panoramas (the original, dominant lights included, and the lighting one without them, see
// gfx::environment::ExtractDominantLights), or the sky for a sun (toward it, in the panorama's space), at its source width
struct Source
{
	const gfx::environment::Panorama* original = nullptr;
	const gfx::environment::Panorama* lighting = nullptr;
	std::optional<glm::vec3> skySun;
	uint32_t skyWidth = gfx::environment::kSkyWidth;
};

// what the kernels drew of the sky: its panorama (without its sun), and its transmittance table (see sky.h), rgb row by row
struct Sky
{
	gfx::environment::Panorama panorama;
	uint32_t transmittanceWidth = 0;
	uint32_t transmittanceHeight = 0;
	std::vector<glm::vec3> transmittance;
};

// the levels and sheen levels (as R16G16B16A16 half floats in data, laid out as gfx::environment::Environment says) and
// the irradiance (dominantLights is left empty: the caller has them), the kernels run on executor's threads; and for the
// sky, what they drew of it into sky (if given)
[[nodiscard]] gfx::environment::Environment Prefilter(
	const Source& source, std::vector<std::byte>& data, core::TaskExecutor& executor, Sky* sky = nullptr);

} // namespace environmentkernels
