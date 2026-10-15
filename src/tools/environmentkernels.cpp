#include "environmentkernels.h"
#include "environmentkernelsbridge.h"

#include <core/taskexecutor.h>

#include <glm/gtc/packing.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <utility>

namespace environmentkernels
{

gfx::environment::Environment Prefilter(const Source& source, std::vector<std::byte>& data, core::TaskExecutor& executor)
{
	using namespace gfx::environment;

	EnvironmentData environmentData{};
	bridge::Input input{.sourceWidth = source.skyWidth};
	if (source.lighting != nullptr)
	{
		input.width = source.lighting->width;
		input.height = source.lighting->height;
		input.original = source.original->rgb.data();
		input.lighting = source.lighting->rgb.data();
	}
	else if (source.sky != nullptr)
	{
		const auto& sky = *source.sky;
		std::copy_n(glm::value_ptr(sky.zenith), 3, environmentData.skyZenith);
		environmentData.skyZenith[3] = 1.0F;
		std::copy_n(glm::value_ptr(sky.horizon), 3, environmentData.skyHorizon);
		std::copy_n(glm::value_ptr(sky.ground), 3, environmentData.skyGround);
		std::copy_n(glm::value_ptr(sky.sunDirection), 3, environmentData.skySun);
		environmentData.skySun[3] = sky.sunCosRadius;
		environmentData.skySunRadiance[0] = sky.sunRadiance;
	}
	auto output = bridge::Prefilter(
		input,
		kLevelCount,
		environmentData,
		[&executor](uint32_t count, core::std_extra::function_ref<void(uint32_t index)> body)
		{ executor.ForEach(count, [&body](size_t index) { body(static_cast<uint32_t>(index)); }); });

	// as the layout gfx::environment::Environment has: the levels, then the sheen levels, as half floats
	Environment environment;
	uint32_t offset = 0;
	for (auto [levels, out] : {std::pair{&output.levels, &environment.levels}, std::pair{&output.sheenLevels, &environment.sheenLevels}})
		for (const auto& level : *levels)
		{
			auto size = level.width * level.height * 4 * static_cast<uint32_t>(sizeof(uint16_t));
			out->push_back({.width = level.width, .height = level.height, .offset = offset, .size = size});
			offset += size;
		}
	environment.size = offset;
	data.resize(offset);
	for (auto [levels, out] : {std::pair{&output.levels, &environment.levels}, std::pair{&output.sheenLevels, &environment.sheenLevels}})
		for (size_t levelIt = 0; levelIt < levels->size(); levelIt++)
		{
			const auto& texels = (*levels)[levelIt].texels;
			auto* halves = reinterpret_cast<uint16_t*>(data.data() + (*out)[levelIt].offset);
			for (size_t texel = 0; texel < texels.size(); texel++)
				for (size_t c = 0; c < 4; c++)
					halves[(texel * 4) + c] = glm::packHalf1x16(texels[texel][c]);
		}
	for (size_t i = 0; i < 9; i++)
		for (size_t c = 0; c < 4; c++)
			environment.irradiance[i][c] = environmentData.irradiance[i][c];
	return environment;
}

} // namespace environmentkernels
