#include "environmentfilter.h"

#include <gfx/shaderloader.h>
#include <gfx/shaders/capi.h>
#include <gfx/texture.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <format>
#include <span>
#include <vector>

namespace gfx
{

using namespace rhi;

namespace environmentfilter
{

// the layout's compute entry points, in its order (see ComputePipelineVariant)
constexpr ComputePipelineVariant kSky{.entryPoint = 0};
constexpr ComputePipelineVariant kDownsample{.entryPoint = 1};
constexpr ComputePipelineVariant kCopy{.entryPoint = 2};
constexpr ComputePipelineVariant kSpecular{.entryPoint = 3};
constexpr ComputePipelineVariant kSheen{.entryPoint = 4};
constexpr ComputePipelineVariant kIrradiance{.entryPoint = 5};

// the irradiance is projected from the first mip of the source at most this wide (as the cpu reference)
constexpr uint32_t kIrradianceSourceWidth = 128;

// the descriptor set of the layout's resources (set 0 holds the push constants)
constexpr uint32_t kSet = 1;

} // namespace environmentfilter

EnvironmentFilter::EnvironmentFilter(Device& device, Pipeline& pipeline, ShaderLoader& loader, const std::filesystem::path& source)
{
	ZoneScopedN("EnvironmentFilter::EnvironmentFilter");

	myLayout = pipeline.CreateLayout(loader.Load(
		source,
		{
			.sourceLanguage = SLANG_SOURCE_LANGUAGE_SLANG,
			.entryPoints =
				{
					{"EnvironmentSky", SLANG_STAGE_COMPUTE},
					{"EnvironmentDownsample", SLANG_STAGE_COMPUTE},
					{"EnvironmentCopy", SLANG_STAGE_COMPUTE},
					{"EnvironmentSpecular", SLANG_STAGE_COMPUTE},
					{"EnvironmentSheen", SLANG_STAGE_COMPUTE},
					{"EnvironmentIrradiance", SLANG_STAGE_COMPUTE},
				},
			.optimizationLevel = SLANG_OPTIMIZATION_LEVEL_MAXIMAL,
			.debugInfoLevel = SLANG_DEBUG_INFO_LEVEL_MAXIMAL,
		}));
	device.GetPipelineLayoutHandles().emplace(std::hash<std::string_view>{}("Environment"), myLayout);

	mySampler = std::make_unique<SamplerVector>(SamplerVectorCreateDesc{
		device.CreateDeviceObjectCreateDesc("Environment Filter Sampler"),
		{SamplerDesc{
			.addressModeU = AddressMode::kRepeat,
			.addressModeV = AddressMode::kClampToEdge,
			.addressModeW = AddressMode::kClampToEdge}}});
}

EnvironmentFilter::~EnvironmentFilter() = default;

std::shared_ptr<void> EnvironmentFilter::Record(
	CommandBufferHandle cmd, Pipeline& pipeline, const EnvironmentTexture& environment, const Buffer& environmentBuffer) const
{
	using namespace environmentfilter;

	ZoneScopedN("EnvironmentFilter::Record");

	auto& device = pipeline.GetDevice();
	CommandEncoder encoder(cmd);

	// a view of each mip (to write it, or read it by texel), kept with the commands
	auto views = std::make_shared<std::vector<std::shared_ptr<ImageView>>>();
	auto mipView = [&device, &views](const Image& image, uint32_t level)
	{
		return views->emplace_back(std::make_shared<ImageView>(ImageViewCreateDesc{
			device.CreateDeviceObjectCreateDesc(std::format("{} Level {}", image.GetName(), level)),
			image,
			image.GetDesc().format,
			ImageAspect::kColor,
			1,
			{},
			level}));
	};
	auto extentOf = [](const Image& image, uint32_t level) { return image.GetDesc().mipLevels[level].extent; };

	auto& source = *environment.source;
	auto& levels = *environment.texture.image;
	auto& sheenLevels = *environment.sheenTexture.image;
	auto sourceLevelCount = static_cast<uint32_t>(source.GetDesc().mipLevels.size());

	// every image in kGeneral throughout: written as storage, read by texel or sampled
	for (auto* image : {&source, &levels, &sheenLevels})
		image->Transition(cmd, ImageLayout::kGeneral, ImageAspect::kColor);
	if (environment.lighting)
		for (const auto* texture : {&environment.lighting, &environment.original})
			texture->image->Transition(cmd, ImageLayout::kGeneral, ImageAspect::kColor);
	// the irradiance is written into gEnvironment, which InstallEnvironment wrote the rest of just before
	encoder.Barrier(PipelineStage::kTransfer, Access::kTransferWrite, PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite);

	pipeline.BindLayoutAuto(myLayout, PipelineBindPoint::kCompute);
	pipeline.SetDescriptorData("gSampler", ImageBinding{.sampler = (*mySampler)[0]}, kSet);
	pipeline.SetDescriptorData("gEnvironment", BufferBinding{.buffer = environmentBuffer, .offset = 0}, kSet);

	auto general = [](const ImageView& view) { return ImageBinding{.sampler = {}, .imageView = view, .layout = ImageLayout::kGeneral}; };
	// one kernel over an output level: what it reads (by texel), and the roughness it is for
	auto dispatch = [&](ComputePipelineVariant kernel, const ImageView* input, const ImageView& output, Extent2d extent, float roughness)
	{
		if (input != nullptr)
			pipeline.SetDescriptorData("gInput", general(*input), kSet);
		pipeline.SetDescriptorData("gOutput", general(output), kSet);
		pipeline.BindDescriptorSetAuto(cmd, kSet);
		pipeline.BindPipelineAuto(cmd, kernel);
		EnvironmentFilterConstants constants{.roughness = roughness, .sourceLevelCount = sourceLevelCount};
		pipeline.PushConstants(cmd, std::as_bytes(std::span(&constants, 1)));
		encoder.DispatchThreads(pipeline.GetComputeLaunchParameters(kernel), extent.width, extent.height);
		// what one kernel wrote, the next reads
		encoder.Barrier(PipelineStage::kComputeShader, Access::kShaderWrite, PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite);
	};

	// the source's level 0: the procedural sky drawn, or the file's lighting panorama copied; then its pyramid
	std::vector<std::shared_ptr<ImageView>> sourceMips;
	for (uint32_t level = 0; level < sourceLevelCount; level++)
		sourceMips.push_back(mipView(source, level));
	if (environment.lighting)
		dispatch(kCopy, environment.lighting.view.get(), *sourceMips[0], extentOf(source, 0), 0.0F);
	else
		dispatch(kSky, nullptr, *sourceMips[0], extentOf(source, 0), 0.0F);
	for (uint32_t level = 1; level < sourceLevelCount; level++)
		dispatch(kDownsample, sourceMips[level - 1].get(), *sourceMips[level], extentOf(source, level), 0.0F);

	// the levels: level 0 the panorama as it is (a file's original, dominant lights included; the sky without its sun,
	// which the shader draws), then the filtered ones, sampling the whole pyramid
	pipeline.SetDescriptorData("gSource", ImageBinding{.sampler = {}, .imageView = *environment.sourceView, .layout = ImageLayout::kGeneral}, kSet);
	auto levelCount = static_cast<uint32_t>(levels.GetDesc().mipLevels.size());
	auto roughnessOf = [](uint32_t level, uint32_t count) { return static_cast<float>(level) / static_cast<float>(count - 1); };
	dispatch(kCopy, environment.original ? environment.original.view.get() : sourceMips[0].get(), *mipView(levels, 0), extentOf(levels, 0), 0.0F);
	for (uint32_t level = 1; level < levelCount; level++)
		dispatch(kSpecular, nullptr, *mipView(levels, level), extentOf(levels, level), roughnessOf(level, levelCount));
	auto sheenLevelCount = static_cast<uint32_t>(sheenLevels.GetDesc().mipLevels.size());
	for (uint32_t level = 0; level < sheenLevelCount; level++)
		dispatch(kSheen, nullptr, *mipView(sheenLevels, level), extentOf(sheenLevels, level), roughnessOf(level, sheenLevelCount));

	// the irradiance, from a small mip: one group
	uint32_t irradianceLevel = 0;
	while (irradianceLevel + 1 < sourceLevelCount && extentOf(source, irradianceLevel).width > kIrradianceSourceWidth)
		irradianceLevel++;
	pipeline.SetDescriptorData("gInput", general(*sourceMips[irradianceLevel]), kSet);
	pipeline.SetDescriptorData("gOutput", general(*sourceMips[0]), kSet); // unused, but bound
	pipeline.BindDescriptorSetAuto(cmd, kSet);
	pipeline.BindPipelineAuto(cmd, kIrradiance);
	EnvironmentFilterConstants constants{.sourceLevelCount = sourceLevelCount};
	pipeline.PushConstants(cmd, std::as_bytes(std::span(&constants, 1)));
	encoder.Dispatch(1, 1, 1);

	// for the frames that sample the levels and read the irradiance
	encoder.Barrier(
		PipelineStage::kComputeShader,
		Access::kShaderWrite,
		PipelineStage::kFragmentShader | PipelineStage::kComputeShader,
		Access::kShaderRead);
	for (auto* image : {&levels, &sheenLevels})
		image->Transition(cmd, ImageLayout::kShaderReadOnly, ImageAspect::kColor);

	for (auto& view : sourceMips)
		views->push_back(std::move(view));
	return views;
}

} // namespace gfx
