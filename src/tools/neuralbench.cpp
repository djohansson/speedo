// neuralbench [shader dir [backend...]]: what a neural radiance cache costs in plain compute, on each backend the build
// has or those named (vulkan, metal, d3d12), on a headless RHI: a multiresolution hash grid of learned features decoded
// by a small network (tools/neuralbench.slang), of a few sizes (kConfigs), queried at the counts a frame would (every
// sixteenth pixel of 1920x1080, every fourth, every one) and trained on a made up scene: the time of a query pass and of
// a training step (and of its passes), and how far the answers are from the scene after so many steps; and the same of
// a cache without learning, given the same samples (cells of running means, with the grid alone). the gate of the
// neural cache's plan: whether what every backend can run (no tensor cores, no cooperative vectors or matrices) is
// practically useful. Run from the checkout's root (the shader dir is src/tools by default), on a gpu doing nothing
// else; Vulkan's driver as for the client (VK_DRIVER_FILES).
#include <core/application.h>
#include <gfx/shaderloader.h>
#include <rhi/buffer.h>
#include <rhi/commandencoder.h>
#include <rhi/queue.h>
#include <rhi/rhi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
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

class BenchApplication final : public core::Application
{
public:
	BenchApplication(std::string_view name, core::Environment&& env)
	: core::Application(name, std::forward<core::Environment>(env))
	{}
};

constexpr uint32_t kSet = 1; // the kernels' resources' (neuralbench.slang)
constexpr uint32_t kPoolDescriptorCount = 16;
// the kernels, in the order they are compiled
constexpr rhi::ComputePipelineVariant kInit{.entryPoint = 0};
constexpr rhi::ComputePipelineVariant kInfer{.entryPoint = 1};
constexpr rhi::ComputePipelineVariant kValidate{.entryPoint = 2};
constexpr rhi::ComputePipelineVariant kTrain{.entryPoint = 3};
constexpr rhi::ComputePipelineVariant kWeightGradient{.entryPoint = 4};
constexpr rhi::ComputePipelineVariant kWeightStep{.entryPoint = 5};
constexpr rhi::ComputePipelineVariant kGridStep{.entryPoint = 6};
constexpr rhi::ComputePipelineVariant kMeanAdd{.entryPoint = 7};
constexpr rhi::ComputePipelineVariant kMeanInfer{.entryPoint = 8};
constexpr rhi::ComputePipelineVariant kMeanValidate{.entryPoint = 9};
constexpr uint32_t kChunkSampleCount = 256; // NB_CHUNK_SAMPLE_COUNT
// the baseline's cells: its four levels', 16 to 128 a side (see NbMeanEntry)
constexpr uint32_t kMeanEntryCount = 4096 + 32768 + 262144 + 2097152;
constexpr uint32_t kValidationCount = 16384;
constexpr uint32_t kValidationSeed = 0x7fffffffU; // places no training step samples
// a frame's queries: 1920x1080's every sixteenth pixel, every fourth, every one
constexpr std::array<uint32_t, 3> kQueryCounts{129600, 518400, 2073600};
constexpr uint32_t kStepCountMost = 1024;

// a network's size (neuralbench.slang's NB_* definitions)
struct Config
{
	const char* name;
	uint32_t levelCount;
	uint32_t tableSizeLog2;
	uint32_t width;
	uint32_t hiddenCount;
	uint32_t growth16ths;

	[[nodiscard]] uint32_t InputCount() const { return levelCount * 2; }
	[[nodiscard]] uint32_t GridEntryCount() const { return levelCount << tableSizeLog2; }
	[[nodiscard]] uint32_t Weight4Count() const
	{
		return hiddenCount > 0 ? ((width / 4) * InputCount()) + ((hiddenCount - 1) * (width / 4) * width) + width : InputCount();
	}
	[[nodiscard]] uint32_t Activation4Count() const { return (InputCount() / 4) + (hiddenCount * (width / 4)); }
	[[nodiscard]] uint32_t Delta4Count() const { return (hiddenCount * (width / 4)) + 1; }
};

// the grid alone (its features' weighted sum), then networks of 2 layers of 32 and 64 and 4 of 64 (Mueller et al. 2021's
// is 5 of 64, without a grid) over 8 levels of 2^17 entries (16 to 2048 cells a side), and a larger grid (12 levels of
// 2^19, 16 to 1384)
constexpr std::array kConfigs{
	Config{.name = "grid 8x2^17, no layers", .levelCount = 8, .tableSizeLog2 = 17, .width = 32, .hiddenCount = 0, .growth16ths = 32},
	Config{.name = "grid 8x2^17, 2x32", .levelCount = 8, .tableSizeLog2 = 17, .width = 32, .hiddenCount = 2, .growth16ths = 32},
	Config{.name = "grid 8x2^17, 2x64", .levelCount = 8, .tableSizeLog2 = 17, .width = 64, .hiddenCount = 2, .growth16ths = 32},
	Config{.name = "grid 8x2^17, 4x64", .levelCount = 8, .tableSizeLog2 = 17, .width = 64, .hiddenCount = 4, .growth16ths = 32},
	Config{.name = "grid 12x2^19, 2x32", .levelCount = 12, .tableSizeLog2 = 19, .width = 32, .hiddenCount = 2, .growth16ths = 24},
};

struct Constants
{
	uint32_t count;
	uint32_t seed;
	uint32_t step;
	float gridRate;
	float weightRate;
	uint32_t initGrid;
	uint32_t initWeights;
	uint32_t initMeans;
};

// the shader binaries a backend takes
template <GraphicsApi G>
constexpr rhi::ShaderFormat kFormat =
	G == kMetal ? rhi::ShaderFormat::kMsl : (G == kD3D12 ? rhi::ShaderFormat::kDxil : rhi::ShaderFormat::kSpirv16);

// one command buffer on the compute queue, recorded by record, submitted and waited for (as rhitest's)
template <GraphicsApi G, typename Record>
void Submit(rhi::Device<G>& device, Record&& record)
{
	using namespace rhi;

	auto compute = device.GetQueue(rhi::kQueueTypeCompute).Write();
	auto& [queue, submits] = compute->queues.Get();
	auto cmd = queue.GetPool().Begin();
	record(cmd);
	cmd.End();
	queue.EnqueueSubmit(
		QueueDeviceSyncInfo<G>{
			.signalSemaphores = {compute->semaphore},
			.signalSemaphoreValues = {++compute->timeline},
		},
		std::move(cmd));
	submits |= queue.Submit();
	queue.WaitIdle();
	auto& executor = core::Application::Get()->GetExecutor();
	queue.SubmitCallbacks(executor, std::numeric_limits<uint64_t>::max());
	executor.JoinAll();
}

// a network of a size on a backend: its query passes' and its training step's times, and its answers' error after steps
template <GraphicsApi G>
void RunConfig(const char* backend, const std::filesystem::path& shaderDir, rhi::Device<G>& device, const Config& config, uint32_t sampleCount)
{
	using namespace rhi;
	using Clock = std::chrono::steady_clock;

	auto& pipeline = device.GetPipeline();
	gfx::ShaderLoader::SlangConfiguration slang{
		.sourceLanguage = SLANG_SOURCE_LANGUAGE_SLANG,
		.entryPoints =
			{
				{"ComputeInit", SLANG_STAGE_COMPUTE},
				{"ComputeInfer", SLANG_STAGE_COMPUTE},
				{"ComputeValidate", SLANG_STAGE_COMPUTE},
				{"ComputeTrain", SLANG_STAGE_COMPUTE},
				{"ComputeWeightGradient", SLANG_STAGE_COMPUTE},
				{"ComputeWeightStep", SLANG_STAGE_COMPUTE},
				{"ComputeGridStep", SLANG_STAGE_COMPUTE},
				{"ComputeMeanAdd", SLANG_STAGE_COMPUTE},
				{"ComputeMeanInfer", SLANG_STAGE_COMPUTE},
				{"ComputeMeanValidate", SLANG_STAGE_COMPUTE},
			},
		.optimizationLevel = SLANG_OPTIMIZATION_LEVEL_MAXIMAL,
		.preprocessorDefinitions =
			{
				{"NB_LEVEL_COUNT", std::to_string(config.levelCount)},
				{"NB_TABLE_SIZE_LOG2", std::to_string(config.tableSizeLog2)},
				{"NB_WIDTH", std::to_string(config.width)},
				{"NB_HIDDEN_COUNT", std::to_string(config.hiddenCount)},
				{"NB_GROWTH_16THS", std::to_string(config.growth16ths)},
			},
	};
	slang.format = kFormat<G>;
	constexpr std::array<uint32_t, 1> kPushedSets{kSet};
	auto layout = pipeline.CreateLayout(
		std::format("NeuralBench {} {}", config.name, sampleCount),
		gfx::ShaderLoader::Compile(shaderDir / "neuralbench.slang", slang, {shaderDir}, std::nullopt),
		kPushedSets);

	const uint32_t gridEntryCount = config.GridEntryCount();
	const uint32_t weight4Count = config.Weight4Count();
	const uint32_t chunkCount = (sampleCount + kChunkSampleCount - 1) / kChunkSampleCount;
	auto buffer = [&device](const char* name, uint64_t size, bool host = false)
	{
		return Buffer<G>(BufferCreateDesc<G>{
			device.CreateDeviceObjectCreateDesc(name),
			size,
			BufferUsage::kStorage,
			host ? MemoryProperty::kHostVisible | MemoryProperty::kHostCoherent : MemoryProperty::kDeviceLocal});
	};
	constexpr uint64_t kFloat4 = 4 * sizeof(float);
	auto grid = buffer("NeuralBench Grid", uint64_t{gridEntryCount} * 2 * sizeof(float));
	auto gridGradient = buffer("NeuralBench Grid Gradient", uint64_t{gridEntryCount} * 2 * sizeof(int32_t));
	auto gridMoments = buffer("NeuralBench Grid Moments", uint64_t{gridEntryCount} * kFloat4);
	auto weights = buffer("NeuralBench Weights", uint64_t{weight4Count} * kFloat4);
	auto weightMoments = buffer("NeuralBench Weight Moments", uint64_t{weight4Count} * 2 * kFloat4);
	auto weightPartials = buffer("NeuralBench Weight Partials", uint64_t{weight4Count} * chunkCount * kFloat4);
	auto activations = buffer("NeuralBench Activations", uint64_t{config.Activation4Count()} * sampleCount * kFloat4);
	auto deltas = buffer("NeuralBench Deltas", uint64_t{config.Delta4Count()} * sampleCount * kFloat4);
	auto output = buffer("NeuralBench Output", uint64_t{kQueryCounts.back()} * kFloat4);
	auto means = buffer("NeuralBench Means", uint64_t{kMeanEntryCount} * 4 * sizeof(uint32_t));
	auto validation = buffer("NeuralBench Validation", uint64_t{kValidationCount} * 2 * kFloat4, true);
	std::ranges::fill(validation.Map(), std::byte{0});

	// the kernels' resources, with the queries' answers or the validation's as their output
	auto bind = [&](CommandBuffer<G>& cmd, Buffer<G>& outputBuffer)
	{
		pipeline.BindLayoutAuto(layout, PipelineBindPoint::kCompute);
		pipeline.SetDescriptorData("gGrid", BufferBinding<G>{.buffer = grid}, kSet);
		pipeline.SetDescriptorData("gGridGradient", BufferBinding<G>{.buffer = gridGradient}, kSet);
		pipeline.SetDescriptorData("gGridMoments", BufferBinding<G>{.buffer = gridMoments}, kSet);
		pipeline.SetDescriptorData("gWeights", BufferBinding<G>{.buffer = weights}, kSet);
		pipeline.SetDescriptorData("gWeightMoments", BufferBinding<G>{.buffer = weightMoments}, kSet);
		pipeline.SetDescriptorData("gWeightPartials", BufferBinding<G>{.buffer = weightPartials}, kSet);
		pipeline.SetDescriptorData("gActivations", BufferBinding<G>{.buffer = activations}, kSet);
		pipeline.SetDescriptorData("gDeltas", BufferBinding<G>{.buffer = deltas}, kSet);
		pipeline.SetDescriptorData("gOutput", BufferBinding<G>{.buffer = outputBuffer}, kSet);
		pipeline.SetDescriptorData("gMeans", BufferBinding<G>{.buffer = means}, kSet);
		pipeline.BindDescriptorSetAuto(cmd, kSet);
	};
	auto dispatch = [&](CommandBuffer<G>& cmd, ComputePipelineVariant kernel, const Constants& constants, uint32_t width, uint32_t height = 1)
	{
		pipeline.BindPipelineAuto(cmd, kernel);
		pipeline.PushConstants(cmd, std::as_bytes(std::span(&constants, 1)));
		CommandEncoder<G>(cmd).DispatchThreads(pipeline.GetComputeLaunchParameters(kernel), width, height);
	};
	// what a pass wrote, before the next reads it
	auto barrier = [](CommandBuffer<G>& cmd)
	{
		CommandEncoder<G>(cmd).Barrier(
			PipelineStage::kComputeShader, Access::kShaderWrite, PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite);
	};
	// a command buffer's time, submitted and waited for, in milliseconds: the least of three
	auto time = [&device](auto&& record)
	{
		double least = std::numeric_limits<double>::max();
		for (int runIt = 0; runIt < 3; runIt++)
		{
			auto start = Clock::now();
			Submit(device, record);
			least = std::min(least, std::chrono::duration<double, std::milli>(Clock::now() - start).count());
		}
		return least;
	};

	// Adam's rates (NEURALBENCH_GRID_RATE, NEURALBENCH_WEIGHT_RATE)
	auto rate = [](const char* name, float value)
	{
		const char* text = std::getenv(name);
		return text != nullptr ? std::strtof(text, nullptr) : value;
	};
	const float gridRate = rate("NEURALBENCH_GRID_RATE", 0.01F);
	const float weightRate = rate("NEURALBENCH_WEIGHT_RATE", 0.003F);
	Submit(
		device,
		[&](CommandBuffer<G>& cmd)
		{
			bind(cmd, output);
			dispatch(
				cmd,
				kInit,
				Constants{.initGrid = gridEntryCount, .initWeights = weight4Count, .initMeans = kMeanEntryCount},
				std::max({gridEntryCount, weight4Count, kMeanEntryCount}));
			barrier(cmd);
		});

	// a step's passes: the samples' answers and gradients, the weights' gradients, the weights' and the grid's steps
	auto step = [&](CommandBuffer<G>& cmd, uint32_t stepIndex)
	{
		Constants constants{.count = sampleCount, .seed = stepIndex + 1, .step = stepIndex, .gridRate = gridRate, .weightRate = weightRate};
		dispatch(cmd, kTrain, constants, sampleCount);
		barrier(cmd);
		dispatch(cmd, kWeightGradient, constants, weight4Count / 4, chunkCount);
		barrier(cmd);
		dispatch(cmd, kWeightStep, constants, weight4Count);
		dispatch(cmd, kGridStep, constants, gridEntryCount);
		barrier(cmd);
	};
	// the answers' error at places no step sampled, against the scene without the estimates' noise: the difference's
	// share of the light, and its mean relative to the light at each place
	struct Error
	{
		double share;
		double relative;
	};
	auto validate = [&](ComputePipelineVariant kernel = kValidate)
	{
		Submit(
			device,
			[&](CommandBuffer<G>& cmd)
			{
				bind(cmd, validation);
				dispatch(cmd, kernel, Constants{.count = kValidationCount, .seed = kValidationSeed}, kValidationCount);
				CommandEncoder<G>(cmd).Barrier(PipelineStage::kComputeShader, Access::kShaderWrite, PipelineStage::kHost, Access::kHostRead);
			});
		auto values = std::span(reinterpret_cast<const float*>(validation.Map().data()), size_t{kValidationCount} * 8);
		double difference = 0.0;
		double light = 0.0;
		double relative = 0.0;
		for (size_t placeIt = 0; placeIt < kValidationCount; placeIt++)
			for (size_t channelIt = 0; channelIt < 3; channelIt++)
			{
				const double answer = values[(placeIt * 8) + channelIt];
				const double target = values[(placeIt * 8) + 4 + channelIt];
				difference += std::abs(answer - target);
				light += target;
				relative += std::abs(answer - target) / (target + 0.01);
			}
		return Error{.share = difference / light, .relative = relative / (3.0 * kValidationCount)};
	};

	std::println(
		"{} | {} | {} grid entries, {} weights, {} samples a step",
		backend,
		config.name,
		gridEntryCount,
		weight4Count * 4,
		sampleCount);
	// the query passes: several a command buffer, so that its submit's own time is little of it
	for (uint32_t queryCount : kQueryCounts)
	{
		const uint32_t passCount = std::max(kQueryCounts.back() / queryCount, 1U) * 4;
		auto record = [&](CommandBuffer<G>& cmd)
		{
			bind(cmd, output);
			for (uint32_t passIt = 0; passIt < passCount; passIt++)
				dispatch(cmd, kInfer, Constants{.count = queryCount, .seed = passIt}, queryCount);
		};
		Submit(device, record); // the pipeline's first use
		std::println("  query {:>7}: {:.3f} ms", queryCount, time(record) / passCount);
	}
	// the training: the error before, then after each doubling of the steps, and the steps' time
	auto before = validate();
	std::println("  steps {:>4}: {:5.1f}% of the light, {:5.1f}% relative", 0, before.share * 100.0, before.relative * 100.0);
	uint32_t done = 0;
	for (uint32_t target = 1; target <= kStepCountMost; target *= 2)
	{
		const uint32_t first = done;
		auto start = Clock::now();
		Submit(
			device,
			[&](CommandBuffer<G>& cmd)
			{
				bind(cmd, output);
				for (uint32_t stepIt = first; stepIt < target; stepIt++)
					step(cmd, stepIt);
			});
		const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
		done = target;
		auto error = validate();
		std::println(
			"  steps {:>4}: {:5.1f}% of the light, {:5.1f}% relative{}",
			done,
			error.share * 100.0,
			error.relative * 100.0,
			target - first >= 32 ? std::format(" ({:.3f} ms a step)", elapsed / (target - first)) : std::string());
	}
	// a step's passes apart, each many times over (what they compute is nothing: the training is done)
	constexpr uint32_t kPassCount = 32;
	Constants constants{.count = sampleCount, .seed = 1, .step = done, .gridRate = 0.0F, .weightRate = 0.0F};
	auto pass = [&](const char* name, ComputePipelineVariant kernel, uint32_t width, uint32_t height = 1)
	{
		std::println(
			"  {}: {:.3f} ms",
			name,
			time(
				[&](CommandBuffer<G>& cmd)
				{
					bind(cmd, output);
					for (uint32_t passIt = 0; passIt < kPassCount; passIt++)
					{
						dispatch(cmd, kernel, constants, width, height);
						barrier(cmd);
					}
				}) /
				kPassCount);
	};
	pass("step's samples (answers, gradients, the grid's sums)", kTrain, sampleCount);
	pass("step's weight gradients", kWeightGradient, weight4Count / 4, chunkCount);
	pass("step's weights", kWeightStep, weight4Count);
	pass("step's grid", kGridStep, gridEntryCount);
	if (config.hiddenCount > 0)
		return;

	// the baseline without learning, once a backend (with the grid alone): cells of running means, given the same samples
	std::println("{} | cells of means, 4 levels (16 to 128 a side) | {} cells, {} samples a step", backend, kMeanEntryCount, sampleCount);
	for (uint32_t queryCount : kQueryCounts)
	{
		const uint32_t passCount = std::max(kQueryCounts.back() / queryCount, 1U) * 4;
		auto record = [&](CommandBuffer<G>& cmd)
		{
			bind(cmd, output);
			for (uint32_t passIt = 0; passIt < passCount; passIt++)
				dispatch(cmd, kMeanInfer, Constants{.count = queryCount, .seed = passIt}, queryCount);
		};
		Submit(device, record);
		std::println("  query {:>7}: {:.3f} ms", queryCount, time(record) / passCount);
	}
	done = 0;
	for (uint32_t target = 1; target <= kStepCountMost; target *= 2)
	{
		const uint32_t first = done;
		auto start = Clock::now();
		Submit(
			device,
			[&](CommandBuffer<G>& cmd)
			{
				bind(cmd, output);
				for (uint32_t stepIt = first; stepIt < target; stepIt++)
				{
					dispatch(cmd, kMeanAdd, Constants{.count = sampleCount, .seed = stepIt + 1}, sampleCount);
					barrier(cmd);
				}
			});
		const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
		done = target;
		auto error = validate(kMeanValidate);
		std::println(
			"  steps {:>4}: {:5.1f}% of the light, {:5.1f}% relative{}",
			done,
			error.share * 100.0,
			error.relative * 100.0,
			target - first >= 32 ? std::format(" ({:.3f} ms a step)", elapsed / (target - first)) : std::string());
	}
}

template <GraphicsApi G>
void Run(const char* backend, const std::filesystem::path& shaderDir, std::string_view only, uint32_t sampleCount)
{
	using namespace rhi;

	RHI<G> rhi(RHIInitializationData{
		.name = "neuralbench",
		.window = kInvalidWindowHandle,
		.descriptorPoolSizes = {
			{.type = rhi::DescriptorType::kUniformBuffer, .count = kPoolDescriptorCount}, // set 0's push constants' block
			{.type = rhi::DescriptorType::kStorageBuffer, .count = kPoolDescriptorCount},
		},
	});
	auto& device = rhi.GetPrimaryDevice();
	for (const auto& config : kConfigs)
		if (only.empty() || std::string_view(config.name).contains(only))
			RunConfig<G>(backend, shaderDir, device, config, sampleCount);
}

} // namespace

int main(int argc, char** argv)
{
	std::filesystem::path shaderDir = std::filesystem::absolute(argc > 1 ? argv[1] : "src/tools");
	std::filesystem::path profile = std::filesystem::temp_directory_path() / "speedo-neuralbench";
	std::filesystem::create_directories(profile);

	core::Environment env;
	env.variables["UserProfilePath"] = profile;
	auto app = core::CreateApplication<BenchApplication>("neuralbench", std::move(env));

	// the backends the arguments after the shader dir name, else every one the build has; NEURALBENCH_CONFIG: only the
	// sizes whose name has it; NEURALBENCH_SAMPLE_COUNT: a step's samples (16384)
	auto runs = [argc, argv](std::string_view backend)
	{
		if (argc <= 2)
			return true;
		for (int argIt = 2; argIt < argc; argIt++)
			if (backend == argv[argIt])
				return true;
		return false;
	};
	const char* onlyText = std::getenv("NEURALBENCH_CONFIG");
	std::string_view only = onlyText != nullptr ? onlyText : "";
	const char* samplesText = std::getenv("NEURALBENCH_SAMPLE_COUNT");
	uint32_t sampleCount = samplesText != nullptr ? std::clamp<uint32_t>(static_cast<uint32_t>(std::strtoul(samplesText, nullptr, 10)), 256, 1U << 20U) : 16384;
	if (runs("vulkan"))
		Run<kVk>("vulkan", shaderDir, only, sampleCount);
#if defined(__OSX__)
	if (runs("metal"))
		Run<kMetal>("metal", shaderDir, only, sampleCount);
#endif
#if defined(__WINDOWS__)
	if (runs("d3d12"))
		Run<kD3D12>("d3d12", shaderDir, only, sampleCount);
#endif
	return 0;
}
