// rhitest [shader dir]: rhi's compute work on each backend the build has (docs/backends-plan.md, steps 3 and 4), on a
// headless RHI: a kernel (tools/rhitest.slang) writing each thread's index times 3 plus a push constant into a storage
// buffer, dispatched twice (its set from the pool, then pushed), and an image kernel (tools/rhitestimage.slang) reading
// an uploaded image loaded and sampled, writing a storage image and a buffer; each read back and checked. Vulkan's is
// the reference; the Metal backend's (on macOS, its kernels compiled to MSL) runs the same, so that it grows behind a
// test of its own while the client stays on Vulkan. Run from the checkout's root (the shader dir is src/tools by
// default); Vulkan's driver as for the client (VK_DRIVER_FILES). Exits 0 when every backend passes.
#include <core/application.h>
#include <gfx/shaderloader.h>
#include <rhi/buffer.h>
#include <rhi/commandencoder.h>
#include <rhi/image.h>
#include <rhi/queue.h>
#include <rhi/rhi.h>
#include <rhi/sampler.h>

#include <array>
#include <cstdint>
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

template <GraphicsApi G>
bool Run(const char* backend, const std::filesystem::path& shaderDir)
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
	return buffers && images;
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

	bool passed = Run<kVk>("vulkan", shaderDir);
#if defined(__OSX__)
	passed = Run<kMetal>("metal", shaderDir) && passed;
#endif
	return passed ? 0 : 1;
}
