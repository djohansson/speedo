#pragma once

#include <gfx/gpu.h>
#include <gfx/texture.h>

#include <filesystem>
#include <memory>

namespace gfx
{

class ShaderLoader;

// prefilters environments on the gpu (environment.slang's kernels, in a layout of their own: "Environment"): the source
// pyramid, the GGX levels, the sheen levels and the irradiance, from the sky (drawn for the sun in gEnvironment, see
// sky.h: its transmittance and multiple scattering tables made once, its view of itself each time) or a file's uploaded
// panoramas (see EnvironmentTexture). gfx::environment::Prefilter is the cpu reference the tests check it against.
class EnvironmentFilter final
{
public:
	EnvironmentFilter(Device& device, Pipeline& pipeline, ShaderLoader& loader, const std::filesystem::path& source);
	~EnvironmentFilter();

	// records an environment's prefiltering into cmd (on the graphics queue, its uploads acquired): for the sky, its view of
	// itself and its panorama first; fills its source pyramid and levels, leaves them shader readable, and writes its
	// irradiance into gEnvironment[0] of environmentBuffer (whose sky fields it reads). the frames before it may still be
	// reading the levels and the sky's view (it waits for them on the gpu), so the sky can be drawn again into the same
	// environment as the sun moves. returns what the commands use beyond the environment (image views of its mips), to
	// keep until the gpu is done with them.
	[[nodiscard]] std::shared_ptr<void> Record(
		CommandBufferHandle cmd, Pipeline& pipeline, const EnvironmentTexture& environment, const Buffer& environmentBuffer);

	// the sky's view of itself (SHADER_TYPES_SKY_VIEW_TEXTURE, see sky.h), in the general layout once the first
	// environment is recorded
	[[nodiscard]] const ImageView& GetSkyView() const { return *mySkyView.view; }

private:
	PipelineLayoutHandle myLayout{};
	std::unique_ptr<SamplerVector> mySampler; // linear, around in u, clamped in v (as the cpu pyramid samples)
	// the sky's tables (see sky.h), storage images always in the general layout, and whether the first two are made
	Texture mySkyTransmittance;
	Texture mySkyMultipleScattering;
	Texture mySkyView;
	bool mySkyTablesMade = false;
};

} // namespace gfx
