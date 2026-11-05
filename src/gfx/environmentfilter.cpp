#include "environmentfilter.h"

#include <gfx/shaderloader.h>
#include <gfx/shaders/capi.h>
#include <gfx/texture.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <format>
#include <span>
#include <utility>
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
constexpr ComputePipelineVariant kSkyTransmittance{.entryPoint = 6};
constexpr ComputePipelineVariant kSkyMultipleScattering{.entryPoint = 7};
constexpr ComputePipelineVariant kSkyView{.entryPoint = 8};
constexpr ComputePipelineVariant kIrradianceRows{.entryPoint = 9};
// the irradiance's partials: a column per spherical harmonics coefficient (see EnvironmentIrradianceRows)
constexpr uint32_t kIrradianceCoefficientCount = 9;

// the irradiance is projected from the first mip of the source at most this wide (as the cpu reference)
constexpr uint32_t kIrradianceSourceWidth = 128;

// the descriptor set of the layout's resources (set 0 holds the push constants)
constexpr uint32_t kSet = 1;

} // namespace environmentfilter

EnvironmentFilter::EnvironmentFilter(Device& device, Pipeline& pipeline, ShaderLoader& loader, const std::filesystem::path& source)
{
	ZoneScopedN("EnvironmentFilter::EnvironmentFilter");

	myLayout = pipeline.CreateLayout(
		"Environment",
		loader.Load(
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
						{"EnvironmentSkyTransmittance", SLANG_STAGE_COMPUTE},
						{"EnvironmentSkyMultipleScattering", SLANG_STAGE_COMPUTE},
						{"EnvironmentSkyView", SLANG_STAGE_COMPUTE},
						{"EnvironmentIrradianceRows", SLANG_STAGE_COMPUTE},
					},
				.optimizationLevel = SLANG_OPTIMIZATION_LEVEL_MAXIMAL,
				.debugInfoLevel = SLANG_DEBUG_INFO_LEVEL_MAXIMAL,
			}));

	mySampler = std::make_unique<SamplerVector>(SamplerVectorCreateDesc{
		device.CreateDeviceObjectCreateDesc("Environment Filter Sampler"),
		{SamplerDesc{
			.addressModeU = AddressMode::kRepeat,
			.addressModeV = AddressMode::kClampToEdge,
			.addressModeW = AddressMode::kClampToEdge}}});

	auto table = [&device](const char* name, uint32_t width, uint32_t height, Format format = Format::kR16G16B16A16Sfloat)
	{
		Texture texture;
		texture.image = std::make_shared<Image>(ImageCreateDesc{
			device.CreateDeviceObjectCreateDesc(name),
			{{.extent = {.width = width, .height = height}}},
			format,
			ImageTiling::kOptimal,
			ImageUsage::kStorage | ImageUsage::kSampled,
			MemoryProperty::kDeviceLocal,
			ImageAspect::kColor,
			ImageLayout::kUndefined});
		texture.view = std::make_shared<ImageView>(
			ImageViewCreateDesc{device.CreateDeviceObjectCreateDesc(std::format("{} View", name)), *texture.image, format, ImageAspect::kColor});
		return texture;
	};
	mySkyTransmittance = table("Sky Transmittance", SHADER_TYPES_SKY_TRANSMITTANCE_WIDTH, SHADER_TYPES_SKY_TRANSMITTANCE_HEIGHT);
	mySkyMultipleScattering = table("Sky Multiple Scattering", SHADER_TYPES_SKY_MULTIPLE_SCATTERING_SIDE, SHADER_TYPES_SKY_MULTIPLE_SCATTERING_SIDE);
	mySkyView = table("Sky View", SHADER_TYPES_SKY_VIEW_WIDTH, SHADER_TYPES_SKY_VIEW_HEIGHT);
	mySkyMoonView = table("Sky Moon View", SHADER_TYPES_SKY_VIEW_WIDTH, SHADER_TYPES_SKY_VIEW_HEIGHT);
	// the irradiance's partials (in floats: a row's sum of a bright panorama's texels passes a half float), a row per row
	// of the source mip it is projected from, at most kIrradianceSourceWidth wide, so half as tall
	myIrradiancePartials = table(
		"Irradiance Partials",
		environmentfilter::kIrradianceCoefficientCount,
		environmentfilter::kIrradianceSourceWidth / 2,
		Format::kR32G32B32A32Sfloat);
}

EnvironmentFilter::~EnvironmentFilter() = default;

std::shared_ptr<void> EnvironmentFilter::Record(
	CommandBufferHandle cmd,
	Pipeline& pipeline,
	const EnvironmentTexture& environment,
	const Buffer& environmentBuffer,
	float skyAerosols,
	GpuProfiler* profiler,
	uint64_t timingGroup)
{
	using namespace environmentfilter;

	ZoneScopedN("EnvironmentFilter::Record");
	// its parts timed with the frame graph's passes (SPEEDO_GPU_TIMINGS), where asked
	static constexpr SourceLocationData kLocation = SOURCE_LOCATION_DATA(skyfilter);
	auto scope = [&](std::string_view name)
	{ return profiler != nullptr && timingGroup != 0 ? profiler->Begin(cmd, name, kLocation, timingGroup) : GpuProfiler::Scope{}; };
	auto wholeScope = scope("Environment Filter");

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

	// the frames before may still read the levels, the sky's view and gEnvironment (a sky drawn again as the sun moves):
	// what this writes waits for them
	encoder.Barrier(PipelineStage::kFragmentShader | PipelineStage::kComputeShader, Access::kNone, PipelineStage::kComputeShader, Access::kNone);
	// every image in kGeneral throughout: written as storage, read by texel or sampled
	for (auto* image :
		 {&source,
		  &levels,
		  &sheenLevels,
		  mySkyTransmittance.image.get(),
		  mySkyMultipleScattering.image.get(),
		  mySkyView.image.get(),
		  mySkyMoonView.image.get(),
		  myIrradiancePartials.image.get()})
		image->Transition(cmd, ImageLayout::kGeneral, ImageAspect::kColor);
	if (environment.lighting)
		for (const auto* texture : {&environment.lighting, &environment.original})
			texture->image->Transition(cmd, ImageLayout::kGeneral, ImageAspect::kColor);
	// the irradiance is written into gEnvironment, which InstallEnvironment wrote the rest of just before
	encoder.Barrier(PipelineStage::kTransfer, Access::kTransferWrite, PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite);

	pipeline.BindLayoutAuto(myLayout, PipelineBindPoint::kCompute);
	pipeline.SetDescriptorData("gSampler", ImageBinding{.sampler = (*mySampler)[0]}, kSet);
	pipeline.SetDescriptorData("gEnvironment", BufferBinding{.buffer = environmentBuffer, .offset = 0}, kSet);
	auto sampled = [](const Texture& texture) { return ImageBinding{.sampler = {}, .imageView = *texture.view, .layout = ImageLayout::kGeneral}; };
	pipeline.SetDescriptorData("gSkyTransmittance", sampled(mySkyTransmittance), kSet);
	pipeline.SetDescriptorData("gSkyMultipleScattering", sampled(mySkyMultipleScattering), kSet);
	pipeline.SetDescriptorData("gSkyView", sampled(mySkyView), kSet);
	pipeline.SetDescriptorData("gSkyMoonView", sampled(mySkyMoonView), kSet);

	auto general = [](const ImageView& view) { return ImageBinding{.sampler = {}, .imageView = view, .layout = ImageLayout::kGeneral}; };
	// one kernel over an output level: what it reads (by texel), the roughness it is for, and the light (the sky's views);
	// then a barrier for the next kernel to read what it wrote, unless the next reads none of it (the kernels of a level
	// each read the pyramid alone: on KosmicKrisp a barrier ends the compute encoder, about a tenth of a millisecond
	// each, and a redraw with one after every kernel cost a time lapse 2 ms a frame more than its kernels do)
	auto dispatch = [&](ComputePipelineVariant kernel,
						const ImageView* input,
						const ImageView& output,
						Extent2d extent,
						float roughness,
						uint32_t light = 0,
						bool readNext = true)
	{
		if (input != nullptr)
			pipeline.SetDescriptorData("gInput", general(*input), kSet);
		pipeline.SetDescriptorData("gOutput", general(output), kSet);
		pipeline.BindDescriptorSetAuto(cmd, kSet);
		pipeline.BindPipelineAuto(cmd, kernel);
		EnvironmentFilterConstants constants{.roughness = roughness, .sourceLevelCount = sourceLevelCount, .light = light};
		pipeline.PushConstants(cmd, std::as_bytes(std::span(&constants, 1)));
		encoder.DispatchThreads(pipeline.GetComputeLaunchParameters(kernel), extent.width, extent.height);
		if (readNext)
			encoder.Barrier(PipelineStage::kComputeShader, Access::kShaderWrite, PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite);
	};

	// the source's level 0: the sky drawn, or the file's lighting panorama copied; then its pyramid
	std::vector<std::shared_ptr<ImageView>> sourceMips;
	for (uint32_t level = 0; level < sourceLevelCount; level++)
		sourceMips.push_back(mipView(source, level));
	// every image binding to this run's images before the first dispatch: a dirty set is updated as a whole, and the
	// previous run's views (another panorama's) are destroyed by now. the sky's first kernel reads no input, and gSource
	// is only set for the levels below, so they still named the previous run's (a validation error, loading a panorama
	// and then the sky into one scene, as the file browser's preview does)
	pipeline.SetDescriptorData("gInput", general(*sourceMips[0]), kSet);
	pipeline.SetDescriptorData("gSource", ImageBinding{.sampler = {}, .imageView = *environment.sourceView, .layout = ImageLayout::kGeneral}, kSet);
	if (environment.lighting)
	{
		dispatch(kCopy, environment.lighting.view.get(), *sourceMips[0], extentOf(source, 0), 0.0F);
	}
	else
	{
		// the sky: its tables the first time, and again when its air changes (its aerosols), its view of itself for the sun
		// now, and its panorama from that
		auto extentOfTable = [](const Texture& texture) { return texture.image->GetDesc().mipLevels[0].extent; };
		if (!mySkyTablesMade || mySkyTablesAerosols != skyAerosols)
		{
			dispatch(kSkyTransmittance, nullptr, *mySkyTransmittance.view, extentOfTable(mySkyTransmittance), 0.0F);
			dispatch(kSkyMultipleScattering, nullptr, *mySkyMultipleScattering.view, extentOfTable(mySkyMultipleScattering), 0.0F);
			mySkyTablesMade = true;
			mySkyTablesAerosols = skyAerosols;
		}
		// the sun's view and the moon's, then the panorama from both
		dispatch(kSkyView, nullptr, *mySkyView.view, extentOfTable(mySkyView), 0.0F, 0, false);
		dispatch(kSkyView, nullptr, *mySkyMoonView.view, extentOfTable(mySkyMoonView), 0.0F, 1);
		dispatch(kSky, nullptr, *sourceMips[0], extentOf(source, 0), 0.0F);
	}
	for (uint32_t level = 1; level < sourceLevelCount; level++)
		dispatch(kDownsample, sourceMips[level - 1].get(), *sourceMips[level], extentOf(source, level), 0.0F);

	// the levels: level 0 the panorama as it is (a file's original, dominant lights included; the sky without its sun,
	// which the shader draws), then the filtered ones, sampling the whole pyramid
	pipeline.SetDescriptorData("gSource", ImageBinding{.sampler = {}, .imageView = *environment.sourceView, .layout = ImageLayout::kGeneral}, kSet);
	auto levelCount = static_cast<uint32_t>(levels.GetDesc().mipLevels.size());
	auto roughnessOf = [](uint32_t level, uint32_t count) { return static_cast<float>(level) / static_cast<float>(count - 1); };
	// the levels, the sheen levels and the irradiance's partials each read the pyramid alone (a file's original for level
	// 0): no barrier between them, one before the irradiance's sums
	{
		dispatch(
			kCopy, environment.original ? environment.original.view.get() : sourceMips[0].get(), *mipView(levels, 0), extentOf(levels, 0), 0.0F, 0, false);
		for (uint32_t level = 1; level < levelCount; level++)
			dispatch(kSpecular, nullptr, *mipView(levels, level), extentOf(levels, level), roughnessOf(level, levelCount), 0, false);
	}
	auto sheenLevelCount = static_cast<uint32_t>(sheenLevels.GetDesc().mipLevels.size());
	{
		for (uint32_t level = 0; level < sheenLevelCount; level++)
			dispatch(kSheen, nullptr, *mipView(sheenLevels, level), extentOf(sheenLevels, level), roughnessOf(level, sheenLevelCount), 0, false);
	}

	// the irradiance, from a small mip: its rows' partials (a thread per coefficient and row), then their sums (one group)
	uint32_t irradianceLevel = 0;
	while (irradianceLevel + 1 < sourceLevelCount && extentOf(source, irradianceLevel).width > kIrradianceSourceWidth)
		irradianceLevel++;
	auto irradianceExtent = extentOf(source, irradianceLevel);
	ENSUREF(
		irradianceExtent.height <= myIrradiancePartials.image->GetDesc().mipLevels[0].extent.height,
		"the irradiance's source mip, {} rows, is taller than its partials hold ({})",
		irradianceExtent.height,
		myIrradiancePartials.image->GetDesc().mipLevels[0].extent.height);
	dispatch(
		kIrradianceRows,
		sourceMips[irradianceLevel].get(),
		*myIrradiancePartials.view,
		Extent2d{.width = kIrradianceCoefficientCount, .height = irradianceExtent.height},
		0.0F);
	pipeline.SetDescriptorData("gInput", sampled(myIrradiancePartials), kSet);
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
	// and the descriptor sets this took (a new one at every dispatch, its images changing), freed once the gpu is done with
	// these commands rather than never: the sky is drawn again every frame while the time of day advances fast
	return std::make_shared<std::pair<decltype(views), std::shared_ptr<void>>>(views, pipeline.TakeDescriptorSets(kSet));
}

} // namespace gfx
