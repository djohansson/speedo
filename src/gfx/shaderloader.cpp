#include <core/assert.h>
#include <core/childprocess.h>
#include <core/dynamiclibrary.h>
#include <core/file.h>
#include <core/path.h>
#include <core/utils.h>
#include <gfx/shadercompiler.h>
#include <gfx/shaderloader.h>
#include <rhi/rhi.h>

#include <picosha2.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <print>
#include <random>
#include <span>
#include <thread>
#include <utility>

namespace gfx::inline SPEEDO_GRAPHICS_API_NAMESPACE
{

namespace shaderloader
{

// the loaders alive: at most one (see ShaderLoader)
static std::atomic_uint32_t gLoaderCount{0};

// the shader compiler library's entry points, loaded at the first compile from beside the executable (built beside the
// client; an install has none, its shaders precompiled), or why it couldn't be
struct Compiler
{
	shadercompiler::CompileFn compile = nullptr;
	shadercompiler::FreeFn free = nullptr;
	std::string error;
};

static const Compiler& GetCompiler()
{
	static const Compiler gCompiler = []
	{
		Compiler compiler;
		auto executable = core::file::GetExecutablePath();
		if (!executable)
		{
			compiler.error = std::format("the executable's path: {}", executable.error().message());
			return compiler;
		}
		auto path = executable->parent_path() / SPEEDO_SHADERCOMPILER_FILE;
		auto library = core::DynamicLibrary::Load(path);
		if (!library)
		{
			compiler.error = library.error();
			return compiler;
		}
		compiler.compile = reinterpret_cast<shadercompiler::CompileFn>(library->Find(shadercompiler::kCompileSymbol));
		compiler.free = reinterpret_cast<shadercompiler::FreeFn>(library->Find(shadercompiler::kFreeSymbol));
		if (compiler.compile == nullptr || compiler.free == nullptr)
			compiler.error = std::format("{} has no {} or {}", path.string(), shadercompiler::kCompileSymbol, shadercompiler::kFreeSymbol);
		return compiler;
	}();
	return gCompiler;
}


// how many worker processes a compile uses (SPEEDO_SHADER_WORKER_COUNT, 0 to 64; 0: the library in this process, one
// request for the whole layout, as before the workers), else half the machine's threads, at most kDefaultWorkerCount
// (each worker holds slang and its own front end of the layout: see docs/shader-compile-plan.md, step 2)
static constexpr uint32_t kDefaultWorkerCount = 8;
static constexpr uint32_t kMaxWorkerCount = 64;

static uint32_t WorkerCount()
{
	static const uint32_t gCount = []
	{
		if (const char* count = std::getenv("SPEEDO_SHADER_WORKER_COUNT"); count != nullptr && *count != '\0')
			return std::min(static_cast<uint32_t>(std::strtoul(count, nullptr, 10)), kMaxWorkerCount);
		return std::clamp(std::thread::hardware_concurrency() / 2, 1U, kDefaultWorkerCount);
	}();
	return gCount;
}

// a key entry point's share of a worker in round one: a front end (half a second) for fewer than this many keys (60 ms
// each) is more than it saves
static constexpr size_t kMinKeysPerWorker = 4;

// where the compiled entry points are kept (see CompileInWorkers): beside the executable, shared by every profile of the
// build (a fresh profile, a test's, compiles nothing a shader edit didn't change), and the workers' request and response
// files under it
static std::filesystem::path CacheRoot()
{
	auto executable = core::file::GetExecutablePath();
	ENSUREF(executable, "the executable's path: {}", executable ? std::string() : executable.error().message());
	return executable->parent_path() / "shader-cache";
}

static std::string RandomHex()
{
	std::random_device device;
	return std::format("{:08x}{:08x}", device(), device());
}

// runs each request in a worker process of its own, all at once, in jobs (removed after); their responses, a hard
// failure where a worker or its compile fails (the compile's diagnostics in the message, as a compile in this process
// gives them)
static std::vector<shadercompiler::Response> RunWorkers(
	std::span<const shadercompiler::Request> requests, const std::filesystem::path& jobs, const std::filesystem::path& file)
{
	auto executable = core::file::GetExecutablePath();
	ENSUREF(executable, "the executable's path: {}", executable ? std::string() : executable.error().message());
	const auto worker = executable->parent_path() / SPEEDO_SHADERCOMPILEWORKER_FILE;
	ENSUREF(
		std::filesystem::exists(worker),
		"{} needs compiling and the shader compiler's worker {} isn't there (an install's shaders come precompiled, and "
		"this one isn't)",
		file.string(),
		worker.string());
	std::error_code error;
	std::filesystem::create_directories(jobs, error);
	ENSUREF(!error, "failed to create {}: {}", jobs.string(), error.message());

	std::vector<core::ChildProcess> processes;
	std::vector<std::filesystem::path> responses;
	for (size_t requestIt = 0; requestIt < requests.size(); requestIt++)
	{
		std::vector<std::byte> bytes;
		ENSUREF(!zpp::bits::failure(zpp::bits::out(bytes)(requests[requestIt])), "failed to serialize the compile of {}", file.string());
		auto requestPath = jobs / std::format("{}.request", requestIt);
		auto& responsePath = responses.emplace_back(jobs / std::format("{}.response", requestIt));
		ENSUREF(core::file::Write(requestPath, bytes), "failed to write {}", requestPath.string());
		std::array<std::string, 2> arguments{requestPath.string(), responsePath.string()};
		auto process = core::ChildProcess::Start(worker, arguments);
		ENSUREF(process, "failed to start {}: {}", worker.string(), process ? std::string() : process.error().message());
		processes.push_back(std::move(*process));
	}
	std::vector<shadercompiler::Response> results(requests.size());
	for (size_t requestIt = 0; requestIt < requests.size(); requestIt++)
	{
		auto exitCode = processes[requestIt].Wait();
		ENSUREF(
			exitCode && *exitCode == 0,
			"the shader compiler's worker failed on {} ({})",
			file.string(),
			exitCode ? std::format("exit code {}", *exitCode) : exitCode.error().message());
		auto mapped = core::file::Map(responses[requestIt]);
		ENSUREF(mapped, "failed to read {}", responses[requestIt].string());
		ENSUREF(
			!zpp::bits::failure(zpp::bits::in(std::span<const std::byte>(mapped->data(), mapped->size()))(results[requestIt])),
			"failed to read the compile of {}",
			file.string());
		// (a failure's diagnostics in its message: see Compile)
		ENSUREF(results[requestIt].ok, "Failed to compile slang file {}:\n{}", file.string(), results[requestIt].diagnostics);
	}
	processes.clear();
	std::filesystem::remove_all(jobs, error);
	return results;
}

// an entry point's compiled code in the cache, by its key (and its specialization's value), or none
static std::optional<rhi::ShaderBinary> ReadCached(const std::filesystem::path& path)
{
	if (!std::filesystem::exists(path))
		return std::nullopt;
	auto mapped = core::file::Map(path);
	if (!mapped || mapped->size() == 0)
		return std::nullopt;
	rhi::ShaderBinary code(mapped->size());
	std::memcpy(code.data(), mapped->data(), mapped->size());
	return code;
}

// written beside it and renamed into place, so that another process (a test's client compiling the same) never reads
// half of it
static void WriteCached(const std::filesystem::path& path, const rhi::ShaderBinary& code)
{
	auto temporary = path;
	temporary += std::format(".{}", RandomHex());
	ENSUREF(core::file::Write(temporary, std::as_bytes(std::span(code))), "failed to write {}", temporary.string());
	std::error_code error;
	std::filesystem::rename(temporary, path, error);
	if (error)
		std::filesystem::remove(temporary, error);
}

// what each job's code took the last time it was compiled, by entry point and specialization (the cache's costs file:
// a line each, "<name> <value or -1> <seconds>"), for dividing the jobs between the workers
static core::UnorderedMap<std::string, float> ReadCosts(const std::filesystem::path& path)
{
	core::UnorderedMap<std::string, float> costs;
	auto mapped = core::file::Map(path);
	if (!mapped)
		return costs;
	std::string_view text(reinterpret_cast<const char*>(mapped->data()), mapped->size());
	while (!text.empty())
	{
		auto end = text.find('\n');
		auto line = text.substr(0, end);
		text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
		auto split = line.rfind(' ');
		if (split == std::string_view::npos)
			continue;
		float seconds = 0.0F;
		auto [at, parsed] = std::from_chars(line.data() + split + 1, line.data() + line.size(), seconds);
		if (parsed == std::errc())
			costs[std::string(line.substr(0, split))] = seconds;
	}
	return costs;
}

static void WriteCosts(const std::filesystem::path& path, const core::UnorderedMap<std::string, float>& costs)
{
	std::vector<std::string> names;
	for (const auto& [name, seconds] : costs)
		names.push_back(name);
	std::ranges::sort(names);
	std::string text;
	for (const auto& name : names)
		text += std::format("{} {}\n", name, costs.at(name));
	auto temporary = path;
	temporary += std::format(".{}", RandomHex());
	if (core::file::Write(temporary, std::as_bytes(std::span(text))))
	{
		std::error_code error;
		std::filesystem::rename(temporary, path, error);
	}
}

// a layout compiled in worker processes (see WorkerCount; docs/shader-compile-plan.md, steps 1 and 2): round one keys
// every entry point (each worker a share, the first reflecting the resources too: see shadercompiler::Action::kKeys);
// the entry points whose code the cache holds by their keys are taken from it; round two compiles the others (and for
// DXIL each specialization of the entry points that read the constant) in the workers at once, divided by what each
// took the last time (or by the size of its source), and puts them in the cache
static rhi::ShaderSet CompileInWorkers(
	const std::filesystem::path& file,
	const ShaderLoader::SlangConfiguration& config,
	shadercompiler::Request request,
	uint32_t workerCount,
	std::vector<std::filesystem::path>* dependencies)
{
	const auto start = std::chrono::steady_clock::now();
	const auto cacheRoot = CacheRoot();
	const auto jobs = cacheRoot / "jobs" / RandomHex();
	const size_t entryCount = request.entryPoints.size();

	// round one: the keys, and the reflection
	const size_t keyWorkerCount = std::clamp<size_t>(entryCount / kMinKeysPerWorker, 1, workerCount);
	std::vector<shadercompiler::Request> keyRequests(keyWorkerCount, request);
	for (size_t workerIt = 0; workerIt < keyWorkerCount; workerIt++)
	{
		keyRequests[workerIt].action = shadercompiler::Action::kKeys;
		keyRequests[workerIt].reflect = workerIt == 0;
		for (size_t entryPointIt = workerIt; entryPointIt < entryCount; entryPointIt += keyWorkerCount)
			keyRequests[workerIt].keyEntryPoints.push_back(static_cast<uint32_t>(entryPointIt));
	}
	auto keyResponses = RunWorkers(keyRequests, jobs / "keys", file);
	const auto keyed = std::chrono::steady_clock::now();
	auto& reflected = keyResponses[0];
	if (!reflected.diagnostics.empty())
	{
		std::print("{}", reflected.diagnostics);
		std::fflush(stdout);
	}
	std::vector<shadercompiler::EntryKey> keys(entryCount);
	for (const auto& response : keyResponses)
		for (const auto& key : response.keys)
			keys[key.entryPoint] = key;

	// the jobs: each entry point's code (DXIL's that read the constant, as its value 0), and DXIL's specializations, by
	// their cache files (a hash of the key and what else makes the code: the configuration but its entry points and
	// specializations, see ParametersHash)
	auto entryConfig = config;
	entryConfig.entryPoints.clear();
	entryConfig.specializations.clear();
	const auto parameters = ShaderLoader::ParametersHash(entryConfig);
	const bool dxil = request.format == rhi::ShaderFormat::kDxil;
	std::vector<uint32_t> values; // the specializations' besides 0, in the configuration's order
	for (uint32_t value : config.specializations)
		if (value != 0 && !std::ranges::contains(values, value))
			values.push_back(value);
	struct Job
	{
		shadercompiler::CodeJob job;
		std::filesystem::path path;
		std::string costName;
		std::optional<rhi::ShaderBinary> code;
	};
	std::vector<Job> allJobs;
	auto addJob = [&](uint32_t entryPoint, bool specialized, uint32_t value)
	{
		const auto& key = keys[entryPoint];
		std::string text = std::format("{}|{:016x}{:016x}|{}", parameters, key.high, key.low, specialized ? std::format("v{}", value) : "plain");
		std::array<uint8_t, 32> sha2{};
		picosha2::hash256(text.cbegin(), text.cend(), sha2.begin(), sha2.end());
		std::string hex;
		picosha2::bytes_to_hex_string(sha2.cbegin(), sha2.cend(), hex);
		auto path = cacheRoot / std::format("{}.bin", hex);
		allJobs.push_back(
			{.job = {.entryPoint = entryPoint, .specialized = specialized, .value = value},
			 .path = path,
			 .costName = std::format("{} {}", request.entryPoints[entryPoint].name, specialized ? static_cast<int64_t>(value) : -1),
			 .code = ReadCached(path)});
	};
	for (uint32_t entryPointIt = 0; entryPointIt < entryCount; entryPointIt++)
		addJob(entryPointIt, dxil && keys[entryPointIt].readsSpecialization, 0);
	for (uint32_t value : values)
		for (uint32_t entryPointIt = 0; entryPointIt < entryCount; entryPointIt++)
			if (dxil && keys[entryPointIt].readsSpecialization)
				addJob(entryPointIt, true, value);

	// round two: the misses, divided between the workers by their costs (longest first, each to the least loaded)
	std::vector<size_t> misses;
	for (size_t jobIt = 0; jobIt < allJobs.size(); jobIt++)
		if (!allJobs[jobIt].code)
			misses.push_back(jobIt);
	const auto costsPath = cacheRoot / std::format("costs-{}", std::to_underlying(request.format));
	auto costs = ReadCosts(costsPath);
	auto cost = [&](size_t jobIt)
	{
		auto costIt = costs.find(allJobs[jobIt].costName);
		// (one never timed by its source's size: about 10 us a byte, FragmentMain's 200 kB 2 s)
		return costIt != costs.end() ? costIt->second : static_cast<float>(keys[allJobs[jobIt].job.entryPoint].sourceSize) * 1e-5F;
	};
	std::ranges::sort(misses, [&](size_t a, size_t b) { return cost(a) > cost(b); });
	const size_t codeWorkerCount = std::min<size_t>(misses.size(), workerCount);
	std::vector<shadercompiler::Request> codeRequests(codeWorkerCount, request);
	std::vector<std::vector<size_t>> assigned(codeWorkerCount);
	std::vector<float> loads(codeWorkerCount, 0.0F);
	for (auto& codeRequest : codeRequests)
		codeRequest.action = shadercompiler::Action::kCode;
	for (size_t jobIt : misses)
	{
		size_t least = static_cast<size_t>(std::ranges::min_element(loads) - loads.begin());
		loads[least] += cost(jobIt);
		assigned[least].push_back(jobIt);
		codeRequests[least].jobs.push_back(allJobs[jobIt].job);
	}
	if (!codeRequests.empty())
	{
		auto codeResponses = RunWorkers(codeRequests, jobs / "code", file);
		for (size_t workerIt = 0; workerIt < codeWorkerCount; workerIt++)
		{
			auto& response = codeResponses[workerIt];
			ENSUREF(response.codes.size() == assigned[workerIt].size(), "a worker compiled {} of {} jobs of {}", response.codes.size(), assigned[workerIt].size(), file.string());
			for (size_t jobIt = 0; jobIt < assigned[workerIt].size(); jobIt++)
			{
				auto& job = allJobs[assigned[workerIt][jobIt]];
				WriteCached(job.path, response.codes[jobIt]);
				costs[job.costName] = response.jobSeconds[jobIt];
				job.code = std::move(response.codes[jobIt]);
			}
		}
		WriteCosts(costsPath, costs);
		if (shadercompiler::Timings())
			for (size_t workerIt = 0; workerIt < codeWorkerCount; workerIt++)
				std::println("shader timings: {} code worker {} ({} jobs, {:.2f} s estimated):{}", file.filename().string(), workerIt, assigned[workerIt].size(), loads[workerIt], codeResponses[workerIt].timings);
	}
	std::error_code error;
	std::filesystem::remove_all(jobs, error);

	// the set: the reflection's, each entry point's code, and DXIL's specializations (each value's, in the configuration's
	// order, the entry points in theirs)
	rhi::ShaderSet shaderSet = std::move(reflected.shaderSet);
	ENSUREF(shaderSet.shaders.size() == entryCount, "the reflection of {} has {} of {} entry points", file.string(), shaderSet.shaders.size(), entryCount);
	for (size_t entryPointIt = 0; entryPointIt < entryCount; entryPointIt++)
		std::get<0>(shaderSet.shaders[entryPointIt]) = std::move(*allJobs[entryPointIt].code);
	for (size_t jobIt = entryCount; jobIt < allJobs.size(); jobIt++)
		shaderSet.specializations.push_back({.shader = allJobs[jobIt].job.entryPoint, .value = allJobs[jobIt].job.value, .code = std::move(*allJobs[jobIt].code)});
	if (dependencies != nullptr)
		for (const auto& dependency : reflected.dependencies)
		{
			if (shadercompiler::Logging())
				std::println("File include/import: {}", dependency);
			dependencies->emplace_back(dependency);
		}
	if (shadercompiler::Timings())
	{
		const auto end = std::chrono::steady_clock::now();
		std::println(
			"shader timings: {} ({} entry points{}) in {} workers: keys {:.2f} s ({} workers:{}), code {:.2f} s ({} of {} jobs compiled, in {} workers), {:.2f} s in all",
			file.filename().string(),
			entryCount,
			values.empty() || !dxil ? std::string() : std::format(", {} specializations", allJobs.size() - entryCount),
			workerCount,
			std::chrono::duration<double>(keyed - start).count(),
			keyWorkerCount,
			keyResponses[0].timings,
			std::chrono::duration<double>(end - keyed).count(),
			misses.size(),
			allJobs.size(),
			codeWorkerCount,
			std::chrono::duration<double>(end - start).count());
	}
	return shaderSet;
}
} // namespace shaderloader

ShaderLoader::ShaderLoader(std::vector<std::filesystem::path>&& includePaths, std::optional<std::filesystem::path>&& intermediatePath)
	: myIncludePaths(std::move(includePaths))
	, myIntermediatePath(std::move(intermediatePath))
{
	ENSUREF(
		shaderloader::gLoaderCount.fetch_add(1, std::memory_order_acq_rel) == 0,
		"two ShaderLoaders alive at once: slang isn't thread safe, its users must take turns (see ShaderLoader)");
}

ShaderLoader::~ShaderLoader()
{
	shaderloader::gLoaderCount.fetch_sub(1, std::memory_order_acq_rel);
}

std::string ShaderLoader::SlangConfiguration::ToString() const
{
	std::string entryPointsString;
	for (const auto& [name, stage] : entryPoints)
		entryPointsString.append(std::format("[{}, {}]", name, static_cast<SlangStageIntegral>(stage)));

	// the definitions select code (BVH_RAY_QUERY), so a cached compile is only another's with the same ones
	std::string definitionsString;
	for (const auto& [name, value] : preprocessorDefinitions)
		definitionsString.append(std::format("[{}={}]", name, value));

	// and the specializations compiled (DXIL's, see SlangConfiguration::specializations)
	std::string specializationsString;
	for (uint32_t value : specializations)
		specializationsString.append(std::format("[{}]", value));

	return std::format(
		"sourceLanguage: {}, entryPoints: {}, "
		"optimizationLevel: {}, debugInfoLevel: {}, debugInfoFormat: {}, matrixLayoutMode: {}, definitions: {}, specializations: {}",
		static_cast<SlangSourceLanguageIntegral>(sourceLanguage),
		entryPointsString,
		static_cast<SlangOptimizationLevelIntegral>(optimizationLevel),
		static_cast<SlangDebugInfoLevelIntegral>(debugInfoLevel),
		static_cast<SlangDebugInfoFormatIntegral>(debugInfoFormat),
		static_cast<SlangMatrixLayoutModeIntegral>(matrixLayoutMode),
		definitionsString,
		specializationsString);
}

std::string ShaderLoader::ParametersHash(const SlangConfiguration& config)
{
	std::string params;
	params.append("slang-");
	params.append(SPEEDO_SLANG_VERSION); // slang's version (its port's), so upgrading it recompiles
	params.append("|cache-v12"); // bump when the serialized ShaderSet layout changes, to invalidate stale caches
	params.append(std::format("|format-{}", std::to_underlying(config.format)));
	if (config.format == rhi::ShaderFormat::kDxil)
		for (const char* option : shadercompiler::kDxcOptions)
			params.append(std::format("|dxc{}", option)); // what the compiler hands DXC (see kDxcOptions)
	params.append(config.ToString());
	static constexpr size_t kSha2Size = 32;
	std::array<uint8_t, kSha2Size> sha2{};
	picosha2::hash256(params.cbegin(), params.cend(), sha2.begin(), sha2.end());
	std::string hash;
	picosha2::bytes_to_hex_string(sha2.cbegin(), sha2.cend(), hash);
	return hash;
}

std::filesystem::path ShaderLoader::PrecompiledPath(const std::filesystem::path& file, const SlangConfiguration& config)
{
	return file.parent_path() / std::format("{}-{}.shaderset", file.stem().string(), ParametersHash(config));
}

rhi::ShaderSet ShaderLoader::Compile(
	const std::filesystem::path& file,
	const SlangConfiguration& config,
	const std::vector<std::filesystem::path>& includePaths,
	const std::optional<std::filesystem::path>& intermediatePath,
	std::vector<std::filesystem::path>* dependencies)
{
	shadercompiler::Request request{
		.format = config.format,
		.file = file.generic_string(),
		.intermediatePath = intermediatePath ? intermediatePath->generic_string() : std::string{},
		.sourceLanguage = static_cast<int32_t>(config.sourceLanguage),
		.optimizationLevel = static_cast<int32_t>(config.optimizationLevel),
		.debugInfoLevel = static_cast<int32_t>(config.debugInfoLevel),
		.debugInfoFormat = static_cast<int32_t>(config.debugInfoFormat),
		.matrixLayoutMode = static_cast<int32_t>(config.matrixLayoutMode),
		.definitions = config.preprocessorDefinitions,
		.specializations = config.specializations};
	for (const auto& includePath : includePaths)
		request.includePaths.push_back(includePath.generic_string());
	for (const auto& [name, stage] : config.entryPoints)
		request.entryPoints.push_back({.name = name, .stage = static_cast<int32_t>(stage)});

	// in worker processes (the client loads no slang), or with SPEEDO_SHADER_WORKER_COUNT=0 in this one
	if (uint32_t workerCount = shaderloader::WorkerCount(); workerCount > 0)
	{
		request.intermediatePath.clear(); // (the workers dump none)
		return shaderloader::CompileInWorkers(file, config, std::move(request), workerCount, dependencies);
	}

	const auto& compiler = shaderloader::GetCompiler();
	ENSUREF(
		compiler.compile != nullptr,
		"{} needs compiling ({} isn't there: an install's shaders come precompiled, and this one isn't), and the shader "
		"compiler couldn't be loaded: {}",
		file.string(),
		PrecompiledPath(file, config).filename().string(),
		compiler.error);

	std::vector<std::byte> requestBytes;
	ENSUREF(!zpp::bits::failure(zpp::bits::out(requestBytes)(request)), "failed to serialize the compile of {}", file.string());

	std::byte* responseBytes = nullptr;
	size_t responseSize = 0;
	int result = compiler.compile(requestBytes.data(), requestBytes.size(), &responseBytes, &responseSize);
	ENSUREF(result == 0, "the shader compiler failed on {} ({})", file.string(), result);
	shadercompiler::Response response;
	auto deserialized = zpp::bits::in(std::span<const std::byte>(responseBytes, responseSize))(response);
	compiler.free(responseBytes);
	ENSUREF(!zpp::bits::failure(deserialized), "failed to read the compile of {}", file.string());

	// a failure's diagnostics in its message: printed before it, to a piped stdout, they were lost when the assert trapped
	// before the buffer was flushed (a client's log in assettest)
	ENSUREF(response.ok, "Failed to compile slang file {}:\n{}", file.string(), response.diagnostics);
	if (!response.diagnostics.empty())
	{
		std::print("{}", response.diagnostics);
		std::fflush(stdout);
	}

	if (dependencies != nullptr)
		for (const auto& dependency : response.dependencies)
		{
			// recorded in the asset manifest, so editing an included/imported file recompiles. todo: hot reload
			if (shadercompiler::Logging())
				std::println("File include/import: {}", dependency);
			dependencies->emplace_back(dependency);
		}

	return std::move(response.shaderSet);
}

void ShaderLoader::SavePrecompiled(const std::filesystem::path& path, const rhi::ShaderSet& shaderSet)
{
	auto saved = core::file::SaveBinary<false>(
		path,
		[&shaderSet](auto& outStream) -> std::error_code
		{
			if (auto result = outStream(shaderSet); failure(result))
				return std::make_error_code(result);
			return {};
		});
	ENSUREF(saved, "failed to write {}", path.string());
}

rhi::ShaderSet ShaderLoader::Load(const std::filesystem::path& file, const SlangConfiguration& config)
{
	auto shaderSet = rhi::ShaderSet{};

	auto loadBin = [&shaderSet](auto& inStream) -> std::error_code
	{
		if (auto result = inStream(shaderSet); failure(result))
			return std::make_error_code(result);

		return {};
	};

	// an install's, compiled when it was packaged: no compiler needed, nor the sources
	if (auto precompiled = PrecompiledPath(file, config); std::filesystem::exists(precompiled))
	{
		ENSUREF(core::file::LoadBinary<false>(precompiled, loadBin) && !shaderSet.shaders.empty(), "Failed to load {}.", precompiled.string());
		return shaderSet;
	}

	auto saveBin = [&shaderSet](auto& outStream) -> std::error_code
	{
		if (auto result = outStream(shaderSet); failure(result))
			return std::make_error_code(result);

		return {};
	};

	std::vector<std::filesystem::path> dependencies;
	auto compile = [this, &shaderSet, &dependencies, &file, &config](auto& /*todo: use me: in*/) -> std::error_code
	{
		shaderSet = Compile(file, config, myIncludePaths, myIntermediatePath, &dependencies);
		return {};
	};

	auto loadResult = core::file::LoadAsset(
		file, compile, loadBin, saveBin, ParametersHash(config), [&dependencies] { return dependencies; });

	ENSUREF(loadResult && !shaderSet.shaders.empty(), "Failed to load shaders.");

	return shaderSet;
}

} // namespace gfx::inline SPEEDO_GRAPHICS_API_NAMESPACE
