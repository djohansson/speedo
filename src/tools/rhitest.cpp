// rhitest [shader dir]: a compute dispatch through rhi on each backend the build has (docs/backends-plan.md, step 3): a
// headless RHI, a kernel (tools/rhitest.slang) writing each thread's index times 3 plus a push constant into a storage
// buffer, read back and checked, dispatched twice: its set from the pool, then pushed. Vulkan's is the reference; the
// Metal backend's (on macOS, its kernel compiled to MSL)
// runs the same, so that it grows behind a test of its own while the client stays on Vulkan. Run from the checkout's
// root (the shader dir is src/tools by default); Vulkan's driver as for the client (VK_DRIVER_FILES). Exits 0 when every
// backend passes.
#include <core/application.h>
#include <gfx/shaderloader.h>
#include <rhi/buffer.h>
#include <rhi/commandencoder.h>
#include <rhi/queue.h>
#include <rhi/rhi.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <print>
#include <span>
#include <string>
#include <string_view>

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
constexpr uint32_t kSet = 1; // the kernel's resources' (rhitest.slang)
constexpr uint32_t kStorageBufferDescriptorCount = 16; // the pool's: one set of one buffer is taken

struct RhiTestConstants
{
	uint32_t offset;
	uint32_t count;
};

template <GraphicsApi G>
bool Run(const char* backend, const std::filesystem::path& shaderDir)
{
	using namespace rhi;

	RHI<G> rhi(RHIInitializationData{
		.name = "rhitest",
		.window = kInvalidWindowHandle,
		.descriptorPoolSizes = {{.type = rhi::DescriptorType::kStorageBuffer, .count = kStorageBufferDescriptorCount}},
	});
	auto& device = rhi.GetPrimaryDevice();
	auto& pipeline = device.GetPipeline();

	gfx::ShaderLoader::SlangConfiguration config{
		.sourceLanguage = SLANG_SOURCE_LANGUAGE_SLANG,
		.entryPoints = {{"ComputeRhiTest", SLANG_STAGE_COMPUTE}},
		.optimizationLevel = SLANG_OPTIMIZATION_LEVEL_MAXIMAL,
	};
	config.format = G == kMetal ? rhi::ShaderFormat::kMsl : rhi::ShaderFormat::kSpirv16;
	auto shaders = gfx::ShaderLoader::Compile(shaderDir / "rhitest.slang", config, {shaderDir}, std::nullopt);
	constexpr ComputePipelineVariant kKernel{.entryPoint = 0};

	// the kernel twice: its set from the pool, then pushed (Pipeline::CreateLayout's pushDescriptorSets), each writing an
	// output of its own with an offset of its own
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

	{
		auto compute = device.GetQueue(kQueueTypeCompute).Write();
		auto& [queue, submits] = compute->queues.Get();
		auto cmd = queue.GetPool().Begin();
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
		}
		cmd.End();
		queue.EnqueueSubmit(
			QueueDeviceSyncInfo<G>{
				.signalSemaphores = {compute->semaphore},
				.signalSemaphoreValues = {++compute->timeline},
			},
			std::move(cmd));
		submits |= queue.Submit();
		queue.WaitIdle();
	}

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
		gfx::ShaderLoader::SlangConfiguration config{
			.sourceLanguage = SLANG_SOURCE_LANGUAGE_SLANG,
			.entryPoints = {{"ComputeRhiTest", SLANG_STAGE_COMPUTE}},
			.optimizationLevel = SLANG_OPTIMIZATION_LEVEL_MAXIMAL,
		};
		auto spirv = gfx::ShaderLoader::Compile(shaderDir / "rhitest.slang", config, {shaderDir}, std::nullopt);
		config.format = rhi::ShaderFormat::kMsl;
		auto msl = gfx::ShaderLoader::Compile(shaderDir / "rhitest.slang", config, {shaderDir}, std::nullopt);
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
