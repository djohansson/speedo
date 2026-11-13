// rhitest [shader dir]: rhi's compute work on each backend the build has (docs/backends-plan.md, steps 3 and 4), on a
// headless RHI: a kernel (tools/rhitest.slang) writing each thread's index times 3 plus a push constant into a storage
// buffer, dispatched twice (its set from the pool, then pushed), and an image kernel (tools/rhitestimage.slang) reading
// an uploaded image loaded and sampled, writing a storage image and a buffer; each read back and checked. Vulkan's is
// the reference; the Metal backend's (on macOS, its kernels compiled to MSL) runs the same, so that it grows behind a
// test of its own while the client stays on Vulkan. Run from the checkout's root (the shader dir is src/tools by
// default); Vulkan's driver as for the client (VK_DRIVER_FILES). Exits 0 when every backend passes.
#include <core/application.h>
#include <gfx/shaderloader.h>
#include <gfx/shaders/capi.h>
#include <rhi/buffer.h>
#include <rhi/commandencoder.h>
#include <rhi/image.h>
#include <rhi/queue.h>
#include <rhi/renderimageset.h>
#include <rhi/rhi.h>
#include <rhi/sampler.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

class TestApplication final : public core::Application
{
public:
	TestApplication(std::string_view name, core::Environment&& env)
	: core::Application(name, std::forward<core::Environment>(env))
	{}
};

constexpr uint32_t kCount = 1000;
constexpr uint32_t kOffset = 7;
constexpr uint32_t kSet = 1; // the kernels' resources' (rhitest.slang, rhitestimage.slang)
constexpr uint32_t kImageSize = 16;
constexpr uint32_t kPoolDescriptorCount = 16; // of each type the kernels bind: a few sets of each are taken
constexpr rhi::ComputePipelineVariant kKernel{.entryPoint = 0};
constexpr uint32_t kTexelCount = kImageSize * kImageSize;

struct RhiTestConstants
{
	uint32_t offset;
	uint32_t count;
};

struct RhiTestImageConstants
{
	uint32_t width;
	uint32_t height;
};

using Texel = std::array<float, 4>;

template <GraphicsApi G>
rhi::ShaderSet Compile(const std::filesystem::path& shaderDir, std::string_view file, const char* entryPoint)
{
	gfx::ShaderLoader::SlangConfiguration config{
		.sourceLanguage = SLANG_SOURCE_LANGUAGE_SLANG,
		.entryPoints = {{entryPoint, SLANG_STAGE_COMPUTE}},
		.optimizationLevel = SLANG_OPTIMIZATION_LEVEL_MAXIMAL,
	};
	config.format = G == kMetal ? rhi::ShaderFormat::kMsl : rhi::ShaderFormat::kSpirv16;
	return gfx::ShaderLoader::Compile(shaderDir / file, config, {shaderDir}, std::nullopt);
}

// one command buffer on the compute queue, recorded by record (which adds what to run once it is done to callbacks),
// submitted, waited for, and its callbacks run
template <GraphicsApi G, typename Record>
void Submit(rhi::Device<G>& device, Record&& record)
{
	using namespace rhi;

	auto compute = device.GetQueue(kQueueTypeCompute).Write();
	auto& [queue, submits] = compute->queues.Get();
	auto cmd = queue.GetPool().Begin();
	std::vector<core::TaskHandle> callbacks;
	record(cmd, callbacks);
	cmd.End();
	queue.EnqueueSubmit(
		QueueDeviceSyncInfo<G>{
			.signalSemaphores = {compute->semaphore},
			.signalSemaphoreValues = {++compute->timeline},
			.callbacks = std::move(callbacks),
		},
		std::move(cmd));
	submits |= queue.Submit();
	queue.WaitIdle();
	auto& executor = core::Application::Get()->GetExecutor();
	queue.SubmitCallbacks(executor, std::numeric_limits<uint64_t>::max());
	executor.JoinAll();
}

// the buffer kernel twice: its set from the pool, then pushed (Pipeline::CreateLayout's pushDescriptorSets), each
// writing an output of its own with an offset of its own
template <GraphicsApi G>
bool RunBuffers(const char* backend, const std::filesystem::path& shaderDir, rhi::Device<G>& device)
{
	using namespace rhi;

	auto& pipeline = device.GetPipeline();
	auto shaders = Compile<G>(shaderDir, "rhitest.slang", "ComputeRhiTest");
	struct Dispatch
	{
		const char* set;
		PipelineLayoutHandle<G> layout;
		uint32_t offset;
		Buffer<G> output;
	};
	constexpr std::array<uint32_t, 1> kPushedSets{kSet};
	std::array<Dispatch, 2> dispatches{{
		{"pooled", pipeline.CreateLayout("RhiTest", shaders), kOffset, {}},
		{"pushed", pipeline.CreateLayout("RhiTestPushed", shaders, kPushedSets), kOffset * 2, {}},
	}};
	for (auto& dispatch : dispatches)
	{
		dispatch.output = Buffer<G>(BufferCreateDesc<G>{
			device.CreateDeviceObjectCreateDesc(std::format("RhiTest Output ({})", dispatch.set)),
			kCount * sizeof(uint32_t),
			BufferUsage::kStorage | BufferUsage::kTransferDestination,
			MemoryProperty::kHostVisible | MemoryProperty::kHostCoherent});
		std::ranges::fill(dispatch.output.Map(), std::byte{0});
	}

	Submit(
		device,
		[&](CommandBuffer<G>& cmd, std::vector<core::TaskHandle>& /*callbacks*/)
		{
			CommandEncoder<G> encoder(cmd);
			for (auto& dispatch : dispatches)
			{
				pipeline.BindLayoutAuto(dispatch.layout, PipelineBindPoint::kCompute);
				pipeline.SetDescriptorData("gOutput", BufferBinding<G>{.buffer = dispatch.output, .offset = 0}, kSet);
				pipeline.BindDescriptorSetAuto(cmd, kSet);
				pipeline.BindPipelineAuto(cmd, kKernel);
				RhiTestConstants constants{.offset = dispatch.offset, .count = kCount};
				pipeline.PushConstants(cmd, std::as_bytes(std::span(&constants, 1)));
				encoder.DispatchThreads(pipeline.GetComputeLaunchParameters(kKernel), kCount);
			}
			encoder.Barrier(PipelineStage::kComputeShader, Access::kShaderWrite, PipelineStage::kHost, Access::kHostRead);
		});

	bool passed = true;
	for (auto& dispatch : dispatches)
	{
		auto values = std::span(reinterpret_cast<const uint32_t*>(dispatch.output.Map().data()), kCount);
		uint32_t wrong = 0;
		for (uint32_t index = 0; index < kCount; index++)
			if (values[index] != (index * 3) + dispatch.offset)
			{
				if (wrong < 4)
					std::println("{} ({} set): output[{}] = {}, not {}", backend, dispatch.set, index, values[index], (index * 3) + dispatch.offset);
				wrong++;
			}
		std::println("{} {} ({} set): {} of {} values right", wrong == 0 ? "PASS" : "FAIL", backend, dispatch.set, kCount - wrong, kCount);
		passed = passed && wrong == 0;
	}
	return passed;
}

// the image kernel over an uploaded image (each texel (x, y, x + y, 1 + x / 2)): its storage image read back by a copy
// (each texel's channels reversed and doubled), its buffer of the texels sampled at their centers (each the texel)
template <GraphicsApi G>
bool RunImages(const char* backend, const std::filesystem::path& shaderDir, rhi::Device<G>& device)
{
	using namespace rhi;

	auto& pipeline = device.GetPipeline();
	auto layout = pipeline.CreateLayout("RhiTestImage", Compile<G>(shaderDir, "rhitestimage.slang", "ComputeRhiTestImage"));

	std::vector<Texel> texels(kTexelCount);
	for (uint32_t y = 0; y < kImageSize; y++)
		for (uint32_t x = 0; x < kImageSize; x++)
			texels[(y * kImageSize) + x] = {
				static_cast<float>(x), static_cast<float>(y), static_cast<float>(x + y), 1.0F + (0.5F * static_cast<float>(x))};

	auto imageDesc = [&device](const char* name, ImageUsage usage)
	{
		return ImageCreateDesc<G>{
			device.CreateDeviceObjectCreateDesc(name),
			Extent3d{.width = kImageSize, .height = kImageSize, .depth = 1},
			Format::kR32G32B32A32Sfloat,
			ImageTiling::kOptimal,
			usage,
			MemoryProperty::kDeviceLocal,
			ImageAspect::kColor,
			ImageLayout::kUndefined};
	};
	auto buffer = [&device](const char* name, BufferUsage usage)
	{
		Buffer<G> result(BufferCreateDesc<G>{
			device.CreateDeviceObjectCreateDesc(name),
			kTexelCount * sizeof(Texel),
			usage,
			MemoryProperty::kHostVisible | MemoryProperty::kHostCoherent});
		std::ranges::fill(result.Map(), std::byte{0});
		return result;
	};
	SamplerVector<G> samplers(SamplerVectorCreateDesc<G>{
		device.CreateDeviceObjectCreateDesc("RhiTest Sampler"),
		{SamplerDesc{
			.magFilter = Filter::kNearest,
			.minFilter = Filter::kNearest,
			.mipmapFilter = Filter::kNearest,
			.addressModeU = AddressMode::kClampToEdge,
			.addressModeV = AddressMode::kClampToEdge,
			.addressModeW = AddressMode::kClampToEdge}}});
	auto sampled = buffer("RhiTest Sampled", BufferUsage::kStorage);
	auto readback = buffer("RhiTest Readback", BufferUsage::kTransferDestination);
	Image<G> target(imageDesc("RhiTest Target", ImageUsage::kStorage | ImageUsage::kTransferSource));
	ImageView<G> targetView(ImageViewCreateDesc<G>{
		device.CreateDeviceObjectCreateDesc("RhiTest Target View"), target, Format::kR32G32B32A32Sfloat, ImageAspect::kColor});
	std::optional<Image<G>> source;
	std::optional<ImageView<G>> sourceView;

	Submit(
		device,
		[&](CommandBuffer<G>& cmd, std::vector<core::TaskHandle>& callbacks)
		{
			core::TaskCreateInfo<void> uploaded;
			source.emplace(
				imageDesc("RhiTest Source", ImageUsage::kSampled | ImageUsage::kTransferDestination),
				cmd,
				texels.data(),
				texels.size() * sizeof(Texel),
				uploaded);
			callbacks.push_back(uploaded.handle);
			sourceView.emplace(ImageViewCreateDesc<G>{
				device.CreateDeviceObjectCreateDesc("RhiTest Source View"), *source, Format::kR32G32B32A32Sfloat, ImageAspect::kColor});

			CommandEncoder<G> encoder(cmd);
			source->Transition(cmd, ImageLayout::kShaderReadOnly);
			target.Transition(cmd, ImageLayout::kGeneral);
			pipeline.BindLayoutAuto(layout, PipelineBindPoint::kCompute);
			pipeline.SetDescriptorData("gSource", ImageBinding<G>{.imageView = *sourceView, .layout = ImageLayout::kShaderReadOnly}, kSet);
			pipeline.SetDescriptorData("gSampler", ImageBinding<G>{.sampler = samplers[0]}, kSet);
			pipeline.SetDescriptorData("gTarget", ImageBinding<G>{.imageView = targetView, .layout = ImageLayout::kGeneral}, kSet);
			pipeline.SetDescriptorData("gSampled", BufferBinding<G>{.buffer = sampled}, kSet);
			pipeline.BindDescriptorSetAuto(cmd, kSet);
			pipeline.BindPipelineAuto(cmd, kKernel);
			RhiTestImageConstants constants{.width = kImageSize, .height = kImageSize};
			pipeline.PushConstants(cmd, std::as_bytes(std::span(&constants, 1)));
			encoder.DispatchThreads(pipeline.GetComputeLaunchParameters(kKernel), kImageSize, kImageSize);
			target.Transition(cmd, ImageLayout::kTransferSource);
			encoder.CopyImageToBuffer(target, readback);
			encoder.Barrier(
				PipelineStage::kComputeShader | PipelineStage::kTransfer,
				Access::kShaderWrite | Access::kTransferWrite,
				PipelineStage::kHost,
				Access::kHostRead);
		});

	auto check = [backend, &texels](const char* what, Buffer<G>& output, auto&& expected)
	{
		auto values = std::span(reinterpret_cast<const Texel*>(output.Map().data()), kTexelCount);
		uint32_t wrong = 0;
		for (uint32_t index = 0; index < kTexelCount; index++)
			if (Texel want = expected(texels[index]); values[index] != want)
			{
				if (wrong < 4)
					std::println(
						"{} ({}): texel {} = ({}, {}, {}, {}), not ({}, {}, {}, {})",
						backend, what, index, values[index][0], values[index][1], values[index][2], values[index][3], want[0], want[1], want[2], want[3]);
				wrong++;
			}
		std::println("{} {} ({}): {} of {} texels right", wrong == 0 ? "PASS" : "FAIL", backend, what, kTexelCount - wrong, kTexelCount);
		return wrong == 0;
	};
	bool stored = check("storage image", readback, [](const Texel& t) { return Texel{t[3] * 2.0F, t[2] * 2.0F, t[1] * 2.0F, t[0] * 2.0F}; });
	bool sampledRight = check("sampled image", sampled, [](const Texel& t) { return t; });
	return stored && sampledRight;
}

// whether a half float is an infinity or a NaN (its exponent's bits all set): told by its bits, since the profile and
// release builds' -ffast-math makes a test of the float undefined (std::isfinite folds to true)
bool HalfIsFinite(uint16_t half)
{
	return ((half >> 10U) & 0x1fU) != 0x1fU;
}

// a half float's value (the environment's tables and panorama are RGBA16F, as the client's); an infinity or a NaN made
// from its bits (-ffast-math leaves numeric_limits' undefined as well)
float HalfToFloat(uint16_t half)
{
	const uint32_t sign = (half >> 15U) & 1U;
	const uint32_t exponent = (half >> 10U) & 0x1fU;
	const uint32_t mantissa = half & 0x3ffU;
	float value = 0.0F;
	if (exponent == 0)
		value = std::ldexp(static_cast<float>(mantissa), -24);
	else if (exponent == 0x1fU)
		return std::bit_cast<float>((sign << 31U) | 0x7f800000U | (mantissa << 13U));
	else
		value = std::ldexp(static_cast<float>(mantissa | 0x400U), static_cast<int>(exponent) - 25);
	return sign != 0 ? -value : value;
}

// an image the environment kernels wrote, read back: its name, its extent, its texels' channels in floats
struct EnvironmentOutput
{
	std::string name;
	uint32_t width = 0;
	uint32_t height = 0;
	std::vector<float> channels;
	bool finite = true; // no channel an infinity or a NaN (HalfIsFinite)
};

// the client's environment kernels (gfx/shaders/environment.slang, its layout's set pushed at each dispatch as
// gfx::EnvironmentFilter has it) drawing the sky: its transmittance and multiple scattering tables, its views of itself
// for the sun and the moon, and a panorama from them, of the same inputs on every backend (a sun high in the south east,
// a quarter moon), each image read back for main to compare the backends'
template <GraphicsApi G>
std::vector<EnvironmentOutput> RunEnvironment(const std::filesystem::path& shaderDir, rhi::Device<G>& device)
{
	using namespace rhi;

	constexpr ComputePipelineVariant kSky{.entryPoint = 0};
	constexpr ComputePipelineVariant kSkyTransmittance{.entryPoint = 1};
	constexpr ComputePipelineVariant kSkyMultipleScattering{.entryPoint = 2};
	constexpr ComputePipelineVariant kSkyView{.entryPoint = 3};
	constexpr uint32_t kPanoramaWidth = 256;
	constexpr uint32_t kPanoramaHeight = 128;
	constexpr uint32_t kTexelByteCount = 8; // RGBA16F

	auto& pipeline = device.GetPipeline();
	const auto gfxShaderDir = shaderDir.parent_path() / "gfx" / "shaders";
	gfx::ShaderLoader::SlangConfiguration config{
		.sourceLanguage = SLANG_SOURCE_LANGUAGE_SLANG,
		.entryPoints =
			{
				{"EnvironmentSky", SLANG_STAGE_COMPUTE},
				{"EnvironmentSkyTransmittance", SLANG_STAGE_COMPUTE},
				{"EnvironmentSkyMultipleScattering", SLANG_STAGE_COMPUTE},
				{"EnvironmentSkyView", SLANG_STAGE_COMPUTE},
			},
		.optimizationLevel = SLANG_OPTIMIZATION_LEVEL_MAXIMAL,
	};
	config.format = G == kMetal ? ShaderFormat::kMsl : ShaderFormat::kSpirv16;
	auto layout = pipeline.CreateLayout(
		"RhiTestEnvironment",
		gfx::ShaderLoader::Compile(gfxShaderDir / "environment.slang", config, {gfxShaderDir}, std::nullopt),
		std::array{kSet});

	SamplerVector<G> sampler(SamplerVectorCreateDesc<G>{
		device.CreateDeviceObjectCreateDesc("RhiTest Environment Sampler"),
		{SamplerDesc{.addressModeU = AddressMode::kRepeat, .addressModeV = AddressMode::kClampToEdge, .addressModeW = AddressMode::kClampToEdge}}});

	struct Table
	{
		std::string name;
		uint32_t width;
		uint32_t height;
		Image<G> image;
		ImageView<G> view;
		Buffer<G> readback;
	};
	auto table = [&device](std::string name, uint32_t width, uint32_t height)
	{
		Image<G> image(ImageCreateDesc<G>{
			device.CreateDeviceObjectCreateDesc(name),
			Extent3d{.width = width, .height = height, .depth = 1},
			Format::kR16G16B16A16Sfloat,
			ImageTiling::kOptimal,
			ImageUsage::kStorage | ImageUsage::kSampled | ImageUsage::kTransferSource,
			MemoryProperty::kDeviceLocal,
			ImageAspect::kColor,
			ImageLayout::kUndefined});
		ImageView<G> view(ImageViewCreateDesc<G>{
			device.CreateDeviceObjectCreateDesc(std::format("{} View", name)), image, Format::kR16G16B16A16Sfloat, ImageAspect::kColor});
		Buffer<G> readback(BufferCreateDesc<G>{
			device.CreateDeviceObjectCreateDesc(std::format("{} Readback", name)),
			uint64_t{width} * height * kTexelByteCount,
			BufferUsage::kTransferDestination,
			MemoryProperty::kHostVisible | MemoryProperty::kHostCoherent});
		return Table{std::move(name), width, height, std::move(image), std::move(view), std::move(readback)};
	};
	std::vector<Table> tables;
	tables.push_back(table("transmittance", SHADER_TYPES_SKY_TRANSMITTANCE_WIDTH, SHADER_TYPES_SKY_TRANSMITTANCE_HEIGHT));
	tables.push_back(table("multiple scattering", SHADER_TYPES_SKY_MULTIPLE_SCATTERING_SIDE, SHADER_TYPES_SKY_MULTIPLE_SCATTERING_SIDE));
	tables.push_back(table("sun's sky view", SHADER_TYPES_SKY_VIEW_WIDTH, SHADER_TYPES_SKY_VIEW_HEIGHT));
	tables.push_back(table("moon's sky view", SHADER_TYPES_SKY_VIEW_WIDTH, SHADER_TYPES_SKY_VIEW_HEIGHT));
	tables.push_back(table("panorama", kPanoramaWidth, kPanoramaHeight));
	auto& transmittance = tables[0];
	auto& multipleScattering = tables[1];
	auto& sunView = tables[2];
	auto& moonView = tables[3];
	auto& panorama = tables[4];

	Buffer<G> environment(BufferCreateDesc<G>{
		device.CreateDeviceObjectCreateDesc("RhiTest Environment Data"),
		sizeof(EnvironmentData),
		BufferUsage::kStorage,
		MemoryProperty::kHostVisible | MemoryProperty::kHostCoherent});
	{
		EnvironmentData data{};
		auto direction = [](float x, float y, float z, float* out)
		{
			const float length = std::sqrt((x * x) + (y * y) + (z * z));
			out[0] = x / length;
			out[1] = y / length;
			out[2] = z / length;
		};
		constexpr float kCosRadius = 0.99998919F; // the sun's and the moon's angular radius, 0.27 degrees
		direction(0.3F, 0.45F, -0.84F, data.skySun);
		data.skySun[3] = kCosRadius;
		data.skySunRadiance[0] = data.skySunRadiance[1] = data.skySunRadiance[2] = 1000.0F;
		data.skySunRadiance[3] = 1.0F; // the environment is the sky
		data.skyAtmosphere[0] = 1.0F; // Hillaire's aerosols
		direction(-0.4F, 0.3F, 0.86F, data.skyMoon);
		data.skyMoon[3] = kCosRadius;
		data.skyMoonRadiance[0] = data.skyMoonRadiance[1] = data.skyMoonRadiance[2] = 0.01F;
		data.skyMoonRadiance[3] = 0.25F;
		data.skyNight[0] = 1.0F; // the local sidereal time
		data.skyNight[1] = 0.9F; // the latitude
		data.skyNight[2] = static_cast<float>(SHADER_TYPES_ATMOSPHERE_NIGHT_RADIANCE);
		std::memcpy(environment.Map().data(), &data, sizeof(data));
	}

	Submit(
		device,
		[&](CommandBuffer<G>& cmd, std::vector<core::TaskHandle>& /*callbacks*/)
		{
			CommandEncoder<G> encoder(cmd);
			for (auto& output : tables)
				output.image.Transition(cmd, ImageLayout::kGeneral, ImageAspect::kColor);
			auto general = [](const ImageView<G>& view) { return ImageBinding<G>{.imageView = view, .layout = ImageLayout::kGeneral}; };
			pipeline.BindLayoutAuto(layout, PipelineBindPoint::kCompute);
			pipeline.SetDescriptorData("gSampler", ImageBinding<G>{.sampler = sampler[0]}, kSet);
			pipeline.SetDescriptorData("gEnvironment", BufferBinding<G>{.buffer = environment}, kSet);
			pipeline.SetDescriptorData("gSkyTransmittance", general(transmittance.view), kSet);
			pipeline.SetDescriptorData("gSkyMultipleScattering", general(multipleScattering.view), kSet);
			pipeline.SetDescriptorData("gSkyView", general(sunView.view), kSet);
			pipeline.SetDescriptorData("gSkyMoonView", general(moonView.view), kSet);
			pipeline.SetDescriptorData("gInput", general(panorama.view), kSet);
			pipeline.SetDescriptorData("gSource", general(panorama.view), kSet);
			auto dispatch = [&](ComputePipelineVariant kernel, Table& output, uint32_t light)
			{
				pipeline.SetDescriptorData("gOutput", general(output.view), kSet);
				pipeline.BindDescriptorSetAuto(cmd, kSet);
				pipeline.BindPipelineAuto(cmd, kernel);
				EnvironmentFilterConstants constants{.roughness = 0.0F, .sourceLevelCount = 1, .light = light};
				pipeline.PushConstants(cmd, std::as_bytes(std::span(&constants, 1)));
				encoder.DispatchThreads(pipeline.GetComputeLaunchParameters(kernel), output.width, output.height);
				encoder.Barrier(PipelineStage::kComputeShader, Access::kShaderWrite, PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite);
			};
			dispatch(kSkyTransmittance, transmittance, 0);
			dispatch(kSkyMultipleScattering, multipleScattering, 0);
			dispatch(kSkyView, sunView, 0);
			dispatch(kSkyView, moonView, 1);
			dispatch(kSky, panorama, 0);
			for (auto& output : tables)
			{
				output.image.Transition(cmd, ImageLayout::kTransferSource, ImageAspect::kColor);
				encoder.CopyImageToBuffer(output.image, output.readback);
			}
			encoder.Barrier(PipelineStage::kTransfer, Access::kTransferWrite, PipelineStage::kHost, Access::kHostRead);
		});

	std::vector<EnvironmentOutput> outputs;
	for (auto& output : tables)
	{
		auto bytes = output.readback.Map();
		auto halves = std::span(reinterpret_cast<const uint16_t*>(bytes.data()), bytes.size() / sizeof(uint16_t));
		EnvironmentOutput result{.name = output.name, .width = output.width, .height = output.height, .channels = {}};
		result.channels.reserve(halves.size());
		for (auto half : halves)
		{
			result.finite = result.finite && HalfIsFinite(half);
			result.channels.push_back(HalfToFloat(half));
		}
		outputs.push_back(std::move(result));
	}
	return outputs;
}

// a draw through a render pass (tools/rhitestdraw.slang): a 64x64 color target cleared, with a depth buffer cleared to
// the far plane (0: depth reversed), three triangles drawn into it (one big, one behind it, one in front of it), two
// with back faces culled (one wound the other way), and an indirect draw of two (each its record by its first
// instance), the color read back for main to compare the backends'; then the color blitted into targets of other sizes
// (RenderTarget::Blit: a draw on Metal, vkCmdBlitImage on Vulkan), scaled up unevenly with the nearest texel and halved
// with a linear filter, each read back too
constexpr uint32_t kDrawSize = 64;

struct DrawBlit
{
	uint32_t width = 0;
	uint32_t height = 0;
	rhi::Filter filter{};
	const char* name = nullptr;
};
constexpr std::array<DrawBlit, 2> kDrawBlits{{
	{.width = 128, .height = 96, .filter = rhi::Filter::kNearest, .name = "blit, 128x96 nearest"},
	{.width = 32, .height = 32, .filter = rhi::Filter::kLinear, .name = "blit, 32x32 linear"},
}};

struct DrawPixels
{
	std::vector<uint8_t> draw;
	std::array<std::vector<uint8_t>, kDrawBlits.size()> blits;
};

// a color attachment's first level and layer, as a blit names them (Metal's names nothing)
template <GraphicsApi G>
ImageSubresourceLayers<G> ColorLayers()
{
	if constexpr (G == kVk)
		return ImageSubresourceLayers<G>{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};
	else
		return {};
}

template <GraphicsApi G>
DrawPixels RunDraw(const std::filesystem::path& shaderDir, rhi::Device<G>& device)
{
	using namespace rhi;

	// as the shader lays it out, padded to 48 bytes explicitly (see rhitestdraw.slang: the same on both backends)
	struct RhiTestDrawConstants
	{
		std::array<float, 4> color;
		std::array<float, 2> offset;
		std::array<float, 2> scale;
		float depth;
		std::array<float, 3> padding{};
	};
	static_assert(sizeof(RhiTestDrawConstants) == 48);

	auto& pipeline = device.GetPipeline();
	auto drawLayout = [&](const char* name, const char* vertexEntryPoint)
	{
		gfx::ShaderLoader::SlangConfiguration config{
			.sourceLanguage = SLANG_SOURCE_LANGUAGE_SLANG,
			.entryPoints = {{vertexEntryPoint, SLANG_STAGE_VERTEX}, {"FragmentRhiTestDraw", SLANG_STAGE_FRAGMENT}},
			.optimizationLevel = SLANG_OPTIMIZATION_LEVEL_MAXIMAL,
		};
		config.format = G == kMetal ? ShaderFormat::kMsl : ShaderFormat::kSpirv16;
		return pipeline.CreateLayout(name, gfx::ShaderLoader::Compile(shaderDir / "rhitestdraw.slang", config, {shaderDir}, std::nullopt));
	};
	auto layout = drawLayout("RhiTestDraw", "VertexRhiTestDraw");
	auto indirectLayout = drawLayout("RhiTestDrawIndirect", "VertexRhiTestDrawIndirect");

	// an indirect draw's: two records (gDraws), an index buffer of one triangle, and two commands naming a record each by
	// their first instance (index count, instance count, first index, vertex offset, first instance)
	auto hostBuffer = [&device](const char* name, uint64_t size, BufferUsage usage, const void* data)
	{
		Buffer<G> buffer(BufferCreateDesc<G>{
			device.CreateDeviceObjectCreateDesc(name), size, usage, MemoryProperty::kHostVisible | MemoryProperty::kHostCoherent});
		std::memcpy(buffer.Map().data(), data, size);
		return buffer;
	};
	const std::array<RhiTestDrawConstants, 2> records{{
		{.color = {1.0F, 0.0F, 1.0F, 1.0F}, .offset = {-0.75F, 0.55F}, .scale = {0.2F, 0.2F}, .depth = 0.9F},
		{.color = {0.0F, 1.0F, 1.0F, 1.0F}, .offset = {-0.35F, 0.75F}, .scale = {0.15F, 0.15F}, .depth = 0.9F},
	}};
	const std::array<uint32_t, 3> indices{0, 1, 2};
	const std::array<std::array<uint32_t, 5>, 2> commands{{{3, 1, 0, 0, 0}, {3, 1, 0, 0, 1}}};
	auto recordBuffer = hostBuffer("RhiTest Draw Records", sizeof(records), BufferUsage::kStorage, records.data());
	auto indexBuffer = hostBuffer("RhiTest Draw Indices", sizeof(indices), BufferUsage::kIndex, indices.data());
	auto commandBuffer = hostBuffer("RhiTest Draw Commands", sizeof(commands), BufferUsage::kIndirect, commands.data());

	auto attachment = [&device](const char* name, Format format, ImageUsage usage, ImageAspect aspect, uint32_t width = kDrawSize, uint32_t height = kDrawSize)
	{
		return std::make_shared<Image<G>>(ImageCreateDesc<G>{
			device.CreateDeviceObjectCreateDesc(name),
			Extent3d{.width = width, .height = height, .depth = 1},
			format,
			ImageTiling::kOptimal,
			usage,
			MemoryProperty::kDeviceLocal,
			aspect,
			ImageLayout::kUndefined});
	};
	// sampled too: a scaled blit's source (Metal's blit draws with it)
	RenderImageSet<G> target(std::vector{
		attachment(
			"RhiTest Draw Color", Format::kR8G8B8A8Unorm, ImageUsage::kColorAttachment | ImageUsage::kTransferSource | ImageUsage::kSampled,
			ImageAspect::kColor),
		attachment("RhiTest Draw Depth", Format::kD32Sfloat, ImageUsage::kDepthStencilAttachment, ImageAspect::kDepth)});
	// the blits' destinations, read back each
	std::vector<RenderImageSet<G>> blitTargets;
	std::vector<Buffer<G>> blitReadbacks;
	blitTargets.reserve(kDrawBlits.size());
	blitReadbacks.reserve(kDrawBlits.size());
	for (const auto& blit : kDrawBlits)
	{
		blitTargets.emplace_back(std::vector{attachment(
			blit.name, Format::kR8G8B8A8Unorm, ImageUsage::kColorAttachment | ImageUsage::kTransferSource | ImageUsage::kTransferDestination,
			ImageAspect::kColor, blit.width, blit.height)});
		blitReadbacks.emplace_back(BufferCreateDesc<G>{
			device.CreateDeviceObjectCreateDesc(std::format("{} readback", blit.name)),
			uint64_t{blit.width} * blit.height * 4,
			BufferUsage::kTransferDestination,
			MemoryProperty::kHostVisible | MemoryProperty::kHostCoherent});
	}
	target.SetLoadOp(LoadOp::kClear, 0);
	target.SetStoreOp(StoreOp::kStore, 0);
	target.SetClearValue(ClearValue{.color = {0.1F, 0.2F, 0.3F, 1.0F}}, 0);
	target.SetLoadOp(LoadOp::kClear, 1);
	target.SetClearValue(ClearValue{.depth = 0.0F}, 1);
	Buffer<G> readback(BufferCreateDesc<G>{
		device.CreateDeviceObjectCreateDesc("RhiTest Draw Readback"),
		uint64_t{kDrawSize} * kDrawSize * 4,
		BufferUsage::kTransferDestination,
		MemoryProperty::kHostVisible | MemoryProperty::kHostCoherent});

	Submit(
		device,
		[&](CommandBuffer<G>& cmd, std::vector<core::TaskHandle>& /*callbacks*/)
		{
			CommandEncoder<G> encoder(cmd);
			pipeline.BindLayoutAuto(layout, PipelineBindPoint::kGraphics);
			pipeline.SetRenderTarget(target);
			// the attachments in their layouts before the pass (a dynamic rendering pass doesn't transition them)
			target.Transition(cmd, ImageLayout::kColorAttachment, ImageAspect::kColor, 0);
			target.Transition(cmd, ImageLayout::kDepthStencilAttachment, ImageAspect::kDepth, 1);
			target.Begin(cmd, SubpassContents::kInline);
			encoder.SetViewport(Viewport{.width = static_cast<float>(kDrawSize), .height = static_cast<float>(kDrawSize)});
			encoder.SetScissor(Rect{.width = kDrawSize, .height = kDrawSize});
			encoder.SetCullMode(CullMode::kNone);
			encoder.SetFrontFace(FrontFace::kCounterClockwise);
			encoder.SetStencilReference(0);
			pipeline.BindPipelineAuto(cmd, GraphicsPipelineVariant{.blend = {BlendMode::kOpaque}});
			auto draw = [&](const RhiTestDrawConstants& constants)
			{
				pipeline.PushConstants(cmd, std::as_bytes(std::span(&constants, 1)));
				encoder.Draw(3);
			};
			// the big one, one behind it (nearer is greater: depth reversed) and one in front of it, overlapping it
			draw({.color = {1.0F, 1.0F, 1.0F, 1.0F}, .offset = {0.0F, 0.0F}, .scale = {1.0F, 1.0F}, .depth = 0.5F});
			draw({.color = {1.0F, 0.0F, 0.0F, 1.0F}, .offset = {0.2F, 0.2F}, .scale = {0.5F, 0.5F}, .depth = 0.25F});
			draw({.color = {0.0F, 1.0F, 0.0F, 1.0F}, .offset = {-0.3F, -0.1F}, .scale = {0.4F, 0.4F}, .depth = 0.75F});
			// back faces culled: two in the lower right, one wound the other way (mirrored in x), so one of them is culled
			// (the same one on every backend)
			encoder.SetCullMode(CullMode::kBack);
			draw({.color = {0.0F, 0.0F, 1.0F, 1.0F}, .offset = {0.45F, 0.45F}, .scale = {0.25F, 0.25F}, .depth = 0.9F});
			draw({.color = {1.0F, 1.0F, 0.0F, 1.0F}, .offset = {0.75F, 0.75F}, .scale = {-0.2F, 0.2F}, .depth = 0.9F});
			// an indirect draw of two commands, each its record by its first instance, in the lower left
			encoder.SetCullMode(CullMode::kNone);
			pipeline.BindLayoutAuto(indirectLayout, PipelineBindPoint::kGraphics);
			pipeline.SetDescriptorData("gDraws", BufferBinding<G>{.buffer = recordBuffer}, kSet);
			pipeline.BindDescriptorSetAuto(cmd, kSet);
			pipeline.BindPipelineAuto(cmd, GraphicsPipelineVariant{.blend = {BlendMode::kOpaque}});
			encoder.BindIndexBuffer(indexBuffer, 0, IndexType::kUint32);
			encoder.DrawIndexedIndirect(commandBuffer, 0, static_cast<uint32_t>(commands.size()), sizeof(commands[0]));
			target.End(cmd);
			target.Transition(cmd, ImageLayout::kTransferSource, ImageAspect::kColor, 0);
			encoder.CopyImageToBuffer(*target.GetImage(0), readback);
			for (size_t blitIt = 0; blitIt < kDrawBlits.size(); blitIt++)
			{
				auto& blitTarget = blitTargets[blitIt];
				blitTarget.Blit(cmd, target, ColorLayers<G>(), 0, ColorLayers<G>(), 0, kDrawBlits[blitIt].filter);
				blitTarget.Transition(cmd, ImageLayout::kTransferSource, ImageAspect::kColor, 0);
				encoder.CopyImageToBuffer(*blitTarget.GetImage(0), blitReadbacks[blitIt]);
			}
			encoder.Barrier(PipelineStage::kTransfer, Access::kTransferWrite, PipelineStage::kHost, Access::kHostRead);
		});

	auto copy = [](Buffer<G>& buffer)
	{
		auto bytes = buffer.Map();
		return std::vector<uint8_t>(
			reinterpret_cast<const uint8_t*>(bytes.data()), reinterpret_cast<const uint8_t*>(bytes.data()) + bytes.size());
	};
	DrawPixels pixels{.draw = copy(readback)};
	for (size_t blitIt = 0; blitIt < kDrawBlits.size(); blitIt++)
		pixels.blits[blitIt] = copy(blitReadbacks[blitIt]);
	return pixels;
}

// the draw's pixels (or a blit's of them, what): some the clear color, some the triangles', and, given a second backend's,
// the same but for rounding (at most 1 of 255 a channel)
bool CheckDraw(
	const char* backend, const char* what, const std::vector<uint8_t>& pixels, const std::vector<uint8_t>* reference, const char* referenceName)
{
	constexpr std::array<uint8_t, 4> kClear{26, 51, 77, 255}; // 0.1, 0.2, 0.3 in unorm8, rounded
	uint32_t cleared = 0;
	for (size_t pixelIt = 0; pixelIt + 4 <= pixels.size(); pixelIt += 4)
	{
		bool clear = true;
		for (size_t channelIt = 0; channelIt < 4; channelIt++)
			clear = clear && std::abs(int{pixels[pixelIt + channelIt]} - int{kClear[channelIt]}) <= 1;
		cleared += clear ? 1 : 0;
	}
	const uint32_t pixelCount = static_cast<uint32_t>(pixels.size() / 4);
	bool drawn = cleared > 0 && cleared < pixelCount;
	if (reference == nullptr)
	{
		std::println("{} {} ({}): {} of {} pixels drawn", drawn ? "PASS" : "FAIL", backend, what, pixelCount - cleared, pixelCount);
		return drawn;
	}
	uint32_t apart = 0;
	int most = 0;
	for (size_t channelIt = 0; channelIt < pixels.size(); channelIt++)
	{
		const int difference = std::abs(int{pixels[channelIt]} - int{(*reference)[channelIt]});
		most = std::max(most, difference);
		apart += difference > 1 ? 1 : 0;
	}
	bool ok = drawn && apart == 0;
	std::println(
		"{} {} ({}): against {}, {} of {} pixels drawn, {} channels more than 1 apart (at most {})",
		ok ? "PASS" : "FAIL", backend, what, referenceName, pixelCount - cleared, pixelCount, apart, most);
	return ok;
}

// the draw's and its blits' checks (see CheckDraw)
bool CheckDrawPixels(const char* backend, const DrawPixels& pixels, const DrawPixels* reference, const char* referenceName)
{
	bool passed = CheckDraw(backend, "draw", pixels.draw, reference != nullptr ? &reference->draw : nullptr, referenceName);
	for (size_t blitIt = 0; blitIt < kDrawBlits.size(); blitIt++)
		passed = CheckDraw(
					 backend, kDrawBlits[blitIt].name, pixels.blits[blitIt], reference != nullptr ? &reference->blits[blitIt] : nullptr,
					 referenceName) &&
				 passed;
	return passed;
}

// the backend's run: the buffer and image kernels' checks, and the environment kernels' outputs and the draw's pixels
// (for main's comparison)
struct RunResult
{
	bool passed = false;
	std::vector<EnvironmentOutput> environment;
	DrawPixels draw;
};

template <GraphicsApi G>
RunResult Run(const char* backend, const std::filesystem::path& shaderDir)
{
	using namespace rhi;

	RHI<G> rhi(RHIInitializationData{
		.name = "rhitest",
		.window = kInvalidWindowHandle,
		.descriptorPoolSizes = {
			{.type = rhi::DescriptorType::kUniformBuffer, .count = kPoolDescriptorCount}, // set 0's push constants' block
			{.type = rhi::DescriptorType::kStorageBuffer, .count = kPoolDescriptorCount},
			{.type = rhi::DescriptorType::kSampledImage, .count = kPoolDescriptorCount},
			{.type = rhi::DescriptorType::kSampler, .count = kPoolDescriptorCount},
			{.type = rhi::DescriptorType::kStorageImage, .count = kPoolDescriptorCount},
		},
	});
	auto& device = rhi.GetPrimaryDevice();
	bool buffers = RunBuffers<G>(backend, shaderDir, device);
	bool images = RunImages<G>(backend, shaderDir, device);
	auto environment = RunEnvironment<G>(shaderDir, device);
	return {.passed = buffers && images, .environment = std::move(environment), .draw = RunDraw<G>(shaderDir, device)};
}

// the environment kernels' outputs: each finite and not all black, and, given a second backend's, the same within what
// their compilers' math may differ by: each channel's difference in stops (log2 of their ratio, over the channels either
// has above the smallest normal half float), on average, and in all but a few channels. those few: the transmittance's
// top row (the top of the atmosphere) decides whether a ray grazing it crosses the atmosphere at all, which rounding
// flips (KosmicKrisp against the Metal backend: 13 of 48244 channels, in 5 texels, 1 against 0.15 to 0.9; with Metal's
// safe math instead of fast, many more), while the tables made from it agree within 0.003 stops
bool CheckEnvironment(const char* backend, const std::vector<EnvironmentOutput>& outputs, const std::vector<EnvironmentOutput>* reference, const char* referenceName)
{
	constexpr float kMeanStops = 0.01F;
	constexpr float kApartStops = 0.25F;
	constexpr double kApartShare = 0.001;
	constexpr float kSmallest = 6.1e-5F;
	bool passed = true;
	for (size_t outputIt = 0; outputIt < outputs.size(); outputIt++)
	{
		const auto& output = outputs[outputIt];
		const bool finite = output.finite;
		bool lit = false;
		for (float value : output.channels)
			lit = lit || value > 0.0F;
		if (reference == nullptr)
		{
			bool ok = finite && lit;
			std::println("{} {} (environment, {}): {}", ok ? "PASS" : "FAIL", backend, output.name, finite ? (lit ? "finite, lit" : "all black") : "not finite");
			passed = passed && ok;
			continue;
		}
		const auto& other = (*reference)[outputIt];
		double sum = 0.0;
		float most = 0.0F;
		size_t count = 0;
		size_t apart = 0;
		for (size_t channelIt = 0; channelIt < output.channels.size(); channelIt++)
		{
			if (channelIt % 4 == 3) // alpha: unused by the sky's
				continue;
			const float value = output.channels[channelIt];
			const float otherValue = other.channels[channelIt];
			if (value < kSmallest && otherValue < kSmallest)
				continue;
			const float stops = std::abs(std::log2((std::max(value, kSmallest)) / std::max(otherValue, kSmallest)));
			sum += stops;
			most = std::max(most, stops);
			apart += stops > kApartStops ? 1 : 0;
			count++;
		}
		const float mean = count > 0 ? static_cast<float>(sum / static_cast<double>(count)) : 0.0F;
		bool ok = finite && lit && mean <= kMeanStops && static_cast<double>(apart) <= kApartShare * static_cast<double>(count);
		std::println(
			"{} {} (environment, {}): against {}, {:.5f} stops mean, {:.5f} most, {} of {} channels over {} stops{}",
			ok ? "PASS" : "FAIL",
			backend,
			output.name,
			referenceName,
			mean,
			most,
			apart,
			count,
			kApartStops,
			finite ? (lit ? "" : ", all black") : ", not finite");
		passed = passed && ok;
	}
	return passed;
}

} // namespace

int main(int argc, char** argv)
{
	std::filesystem::path shaderDir = std::filesystem::absolute(argc > 1 ? argv[1] : "src/tools");
	std::filesystem::path profile = std::filesystem::temp_directory_path() / "speedo-rhitest";
	std::filesystem::create_directories(profile);

	core::Environment env;
	env.variables["UserProfilePath"] = profile;
	auto app = core::CreateApplication<TestApplication>("rhitest", std::move(env));

	// the kernel compiled to MSL has the layouts spir-v's has (the shader compiler reflects the spir-v target for both,
	// see shadercompiler.cpp): a set, a binding, a buffer, and the push constants
	{
		auto spirv = Compile<kVk>(shaderDir, "rhitest.slang", "ComputeRhiTest");
		auto msl = Compile<kMetal>(shaderDir, "rhitest.slang", "ComputeRhiTest"); // the format alone (any platform)
		auto bindings = [](const rhi::ShaderSet& set)
		{
			std::string text;
			for (const auto& [index, layout] : set.layouts)
				for (const auto& binding : layout.bindings)
					text += std::format("{}:{}:{}:{} ", index, binding.binding, binding.name, binding.count);
			return text;
		};
		std::string_view source(std::get<0>(msl.shaders.front()).data(), std::get<0>(msl.shaders.front()).size());
		bool same = bindings(spirv) == bindings(msl) && source.contains("[[kernel]]");
		std::println("{} msl: layouts {}, the kernel's MSL {} bytes", same ? "PASS" : "FAIL", bindings(msl), source.size());
		if (!same)
			return 1;
	}

	auto vulkan = Run<kVk>("vulkan", shaderDir);
	bool passed = vulkan.passed && CheckEnvironment("vulkan", vulkan.environment, nullptr, nullptr);
	passed = CheckDrawPixels("vulkan", vulkan.draw, nullptr, nullptr) && passed;
#if defined(__OSX__)
	auto metal = Run<kMetal>("metal", shaderDir);
	passed = metal.passed && CheckEnvironment("metal", metal.environment, &vulkan.environment, "vulkan") && passed;
	passed = CheckDrawPixels("metal", metal.draw, &vulkan.draw, "vulkan") && passed;
#endif
	return passed ? 0 : 1;
}
