// rhitest [shader dir]: a compute dispatch through rhi on each backend the build has (docs/backends-plan.md, step 3): a
// headless RHI, a kernel (tools/rhitest.slang) writing each thread's index times 3 plus a push constant into a storage
// buffer, read back and checked. Vulkan's is the reference; the Metal backend's joins as its functions arrive, so that
// it grows behind a test of its own while the client stays on Vulkan. Run from the checkout's root (the shader dir is
// src/tools by default); the driver as for the client (VK_DRIVER_FILES). Exits 0 when every backend passes.
#include <core/application.h>
#include <gfx/shaderloader.h>
#include <rhi/buffer.h>
#include <rhi/commandencoder.h>
#include <rhi/queue.h>
#include <rhi/rhi.h>

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
	auto layout = pipeline.CreateLayout(
		"RhiTest", gfx::ShaderLoader::Compile(shaderDir / "rhitest.slang", config, {shaderDir}, std::nullopt));
	constexpr ComputePipelineVariant kKernel{.entryPoint = 0};

	Buffer<G> output(BufferCreateDesc<G>{
		device.CreateDeviceObjectCreateDesc("RhiTest Output"),
		kCount * sizeof(uint32_t),
		BufferUsage::kStorage | BufferUsage::kTransferDestination,
		MemoryProperty::kHostVisible | MemoryProperty::kHostCoherent});
	std::ranges::fill(output.Map(), std::byte{0});

	{
		auto compute = device.GetQueue(kQueueTypeCompute).Write();
		auto& [queue, submits] = compute->queues.Get();
		auto cmd = queue.GetPool().Begin();
		{
			CommandEncoder<G> encoder(cmd);
			pipeline.BindLayoutAuto(layout, PipelineBindPoint::kCompute);
			pipeline.SetDescriptorData("gOutput", BufferBinding<G>{.buffer = output, .offset = 0}, kSet);
			pipeline.BindDescriptorSetAuto(cmd, kSet);
			pipeline.BindPipelineAuto(cmd, kKernel);
			RhiTestConstants constants{.offset = kOffset, .count = kCount};
			pipeline.PushConstants(cmd, std::as_bytes(std::span(&constants, 1)));
			encoder.DispatchThreads(pipeline.GetComputeLaunchParameters(kKernel), kCount);
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

	auto values = std::span(reinterpret_cast<const uint32_t*>(output.Map().data()), kCount);
	uint32_t wrong = 0;
	for (uint32_t index = 0; index < kCount; index++)
		if (values[index] != (index * 3) + kOffset)
		{
			if (wrong < 4)
				std::println("{}: output[{}] = {}, not {}", backend, index, values[index], (index * 3) + kOffset);
			wrong++;
		}
	std::println("{} {}: {} of {} values right", wrong == 0 ? "PASS" : "FAIL", backend, kCount - wrong, kCount);
	return wrong == 0;
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
	return passed ? 0 : 1;
}
