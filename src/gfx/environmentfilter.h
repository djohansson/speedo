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
	// keep until the gpu is done with them. skyAerosols: what gEnvironment[0] says of them (EnvironmentData::skyAtmosphere),
	// for which the sky's tables are made again if they were made for other. profiler and timingGroup (the queue's, and
	// the renderer's group, see Renderer::GetTimingGroup): its parts timed as the frame graph's passes are, for
	// SPEEDO_GPU_TIMINGS (none without)
	[[nodiscard]] std::shared_ptr<void> Record(
		CommandBufferHandle cmd,
		Pipeline& pipeline,
		const EnvironmentTexture& environment,
		const Buffer& environmentBuffer,
		float skyAerosols = 1.0F,
		GpuProfiler* profiler = nullptr,
		uint64_t timingGroup = 0);

	// the sky's view of itself (SHADER_TYPES_SKY_VIEW_TEXTURE, see sky.h), in the general layout once the first
	// environment is recorded
	[[nodiscard]] const ImageView& GetSkyView() const { return *mySkyView.view; }
	// and lit by the moon (SHADER_TYPES_SKY_MOON_VIEW_TEXTURE), per unit of its illuminance, made with the sun's
	[[nodiscard]] const ImageView& GetSkyMoonView() const { return *mySkyMoonView.view; }
	// and its other tables (SHADER_TYPES_SKY_TRANSMITTANCE_TEXTURE, SHADER_TYPES_SKY_MULTIPLE_SCATTERING_TEXTURE), which the
	// aerial perspective is made from: the same, once the sky is
	[[nodiscard]] const ImageView& GetSkyTransmittance() const { return *mySkyTransmittance.view; }
	[[nodiscard]] const ImageView& GetSkyMultipleScattering() const { return *mySkyMultipleScattering.view; }

private:
	PipelineLayoutHandle myLayout{};
	std::unique_ptr<SamplerVector> mySampler; // linear, around in u, clamped in v (as the cpu pyramid samples)
	// the sky's tables (see sky.h), storage images always in the general layout, and whether the first two are made, for
	// how much of the aerosols (see EnvironmentData::skyAtmosphere)
	Texture mySkyTransmittance;
	Texture mySkyMultipleScattering;
	Texture mySkyView;
	Texture mySkyMoonView;
	Texture myIrradiancePartials; // see EnvironmentIrradianceRows
	bool mySkyTablesMade = false;
	float mySkyTablesAerosols = 1.0F;
};

} // namespace gfx
