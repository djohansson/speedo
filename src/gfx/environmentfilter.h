#pragma once

#include <gfx/gpu.h>

#include <filesystem>
#include <memory>

namespace gfx
{

class ShaderLoader;
struct EnvironmentTexture;

// prefilters environments on the gpu (environment.slang's kernels, in a layout of their own: "Environment"): the source
// pyramid, the GGX levels, the sheen levels and the irradiance, from the procedural sky's parameters or a file's uploaded
// panoramas (see EnvironmentTexture). gfx::environment::Prefilter is the cpu reference the tests check it against.
class EnvironmentFilter final
{
public:
	EnvironmentFilter(Device& device, Pipeline& pipeline, ShaderLoader& loader, const std::filesystem::path& source);
	~EnvironmentFilter();

	// records an environment's prefiltering into cmd (on the graphics queue, its uploads acquired): fills its source
	// pyramid and levels, leaves them shader readable, and writes its irradiance into gEnvironment[0] of environmentBuffer
	// (whose sky parameters it reads). returns what the commands use beyond the environment (image views of its mips),
	// to keep until the gpu is done with them.
	[[nodiscard]] std::shared_ptr<void> Record(
		CommandBufferHandle cmd, Pipeline& pipeline, const EnvironmentTexture& environment, const Buffer& environmentBuffer) const;

private:
	PipelineLayoutHandle myLayout{};
	std::unique_ptr<SamplerVector> mySampler; // linear, around in u, clamped in v (as the cpu pyramid samples)
};

} // namespace gfx
