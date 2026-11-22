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
#include <xxhash.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <optional>
#include <print>
#include <random>
#include <span>
#include <string_view>
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

// runs each request in a worker process of its own, all at once, in jobs (removed after); their responses, or where a
// worker or its compile fails, why (the compile's diagnostics)
static std::expected<std::vector<shadercompiler::Response>, std::string> RunWorkers(
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
	std::string failure;
	for (size_t requestIt = 0; requestIt < requests.size(); requestIt++)
	{
		// (every worker waited for, whichever failed)
		auto exitCode = processes[requestIt].Wait();
		if (!failure.empty())
			continue;
		if (!exitCode || *exitCode != 0)
		{
			failure = std::format(
				"the shader compiler's worker failed on {} ({})", file.string(), exitCode ? std::format("exit code {}", *exitCode) : exitCode.error().message());
			continue;
		}
		auto mapped = core::file::Map(responses[requestIt]);
		if (!mapped ||
			zpp::bits::failure(zpp::bits::in(std::span<const std::byte>(mapped->data(), mapped->size()))(results[requestIt])))
			failure = std::format("failed to read the compile of {} ({})", file.string(), responses[requestIt].string());
		else if (!results[requestIt].ok)
			failure = std::format("Failed to compile slang file {}:\n{}", file.string(), results[requestIt].diagnostics);
	}
	processes.clear();
	std::filesystem::remove_all(jobs, error);
	if (!failure.empty())
		return std::unexpected(failure);
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

static std::string Sha256Hex(std::string_view text)
{
	std::array<uint8_t, 32> sha2{};
	picosha2::hash256(text.cbegin(), text.cend(), sha2.begin(), sha2.end());
	std::string hex;
	picosha2::bytes_to_hex_string(sha2.cbegin(), sha2.cend(), hex);
	return hex;
}

// bytes written beside path and renamed into place, as WriteCached does
static void WriteReplacing(const std::filesystem::path& path, std::span<const std::byte> bytes)
{
	auto temporary = path;
	temporary += std::format(".{}", RandomHex());
	if (!core::file::Write(temporary, bytes))
		return;
	std::error_code error;
	std::filesystem::rename(temporary, path, error);
	if (error)
		std::filesystem::remove(temporary, error);
}

// round one's results for one variant of the sources (see CompileInWorkers): the files it read, the reflection (the
// resources, the entry points without code) and each entry point's key. kept in the cache by what they were made from,
// so that the sources as they were once (another variant of a shader swapped back in, in any profile) skip the round:
// 4.3 s of front ends and keys for the main layouts on every start after a swap (2026-11-22), every entry point's code
// in the cache
struct KeysRecord
{
	std::vector<std::string> dependencies;
	rhi::ShaderSet shaderSet;
	std::vector<shadercompiler::EntryKey> keys;
};

// a file's size and time, for the compiler's binaries (a rebuilt worker may key or reflect differently)
static std::string FileStamp(const std::filesystem::path& path)
{
	std::error_code error;
	auto size = std::filesystem::file_size(path, error);
	auto time = std::filesystem::last_write_time(path, error);
	return std::format("{}:{}", size, time.time_since_epoch().count());
}

// what a layout's round one depends on besides its sources: the file, its configuration (the entry points, the
// definitions, the format, slang's version: see ParametersHash), the include paths, and the compiler's binaries
static std::string KeysIdentity(const std::filesystem::path& file, const ShaderLoader::SlangConfiguration& config, const shadercompiler::Request& request)
{
	auto executable = core::file::GetExecutablePath();
	ENSUREF(executable, "the executable's path: {}", executable ? std::string() : executable.error().message());
	const auto directory = executable->parent_path();
	std::string text = std::format("keys-v1|{}|{}", ShaderLoader::ParametersHash(config), file.generic_string());
	for (const auto& includePath : request.includePaths)
		text += std::format("|{}", includePath);
	text += std::format("|{}|{}", FileStamp(directory / SPEEDO_SHADERCOMPILEWORKER_FILE), FileStamp(directory / SPEEDO_SHADERCOMPILER_FILE));
	return Sha256Hex(text);
}

// a record's name: the identity, and each dependency's path and contents as they are now; none where one can't be read
static std::optional<std::string> KeysRecordName(std::string_view identity, std::span<const std::string> dependencies)
{
	std::string text(identity);
	for (const auto& dependency : dependencies)
	{
		auto mapped = core::file::Map(dependency);
		if (!mapped)
			return std::nullopt;
		auto hash = XXH3_128bits(mapped->data(), mapped->size());
		text += std::format("|{}|{:016x}{:016x}", dependency, hash.high64, hash.low64);
	}
	return std::format("keys-{}", Sha256Hex(text));
}

// the record of the sources as they are now, if the cache holds it: looked up by the dependencies the last round one of
// this identity read (keys-<identity>.deps, a path a line); a variant that reads other files misses once
static std::optional<KeysRecord> ReadKeysRecord(const std::filesystem::path& cacheRoot, const std::string& identity, size_t entryCount)
{
	auto list = core::file::Map(cacheRoot / std::format("keys-{}.deps", identity));
	if (!list || list->size() == 0)
		return std::nullopt;
	std::vector<std::string> dependencies;
	std::string_view text(reinterpret_cast<const char*>(list->data()), list->size());
	while (!text.empty())
	{
		auto end = text.find('\n');
		if (auto line = text.substr(0, end); !line.empty())
			dependencies.emplace_back(line);
		text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
	}
	auto name = KeysRecordName(identity, dependencies);
	if (!name)
		return std::nullopt;
	auto mapped = core::file::Map(cacheRoot / std::format("{}.bin", *name));
	if (!mapped || mapped->size() == 0)
		return std::nullopt;
	KeysRecord record;
	if (zpp::bits::failure(zpp::bits::in(std::span<const std::byte>(mapped->data(), mapped->size()))(record)))
		return std::nullopt;
	// (its name says these files had these contents: the same list, or a collision)
	if (record.dependencies != dependencies || record.keys.size() != entryCount || record.shaderSet.shaders.size() != entryCount)
		return std::nullopt;
	return record;
}

// keeps round one's record, and its dependencies as the identity's list, unless a dependency was written since the
// round started (an edit during the compile: the front ends may have read either version)
static void WriteKeysRecord(
	const std::filesystem::path& cacheRoot, const std::string& identity, const KeysRecord& record, std::filesystem::file_time_type roundStart)
{
	for (const auto& dependency : record.dependencies)
	{
		std::error_code error;
		auto time = std::filesystem::last_write_time(dependency, error);
		if (error || time >= roundStart)
			return;
	}
	auto name = KeysRecordName(identity, record.dependencies);
	if (!name)
		return;
	std::vector<std::byte> bytes;
	if (zpp::bits::failure(zpp::bits::out(bytes)(record)))
		return;
	WriteReplacing(cacheRoot / std::format("{}.bin", *name), bytes);
	std::string list;
	for (const auto& dependency : record.dependencies)
		list += std::format("{}\n", dependency);
	WriteReplacing(cacheRoot / std::format("keys-{}.deps", identity), std::as_bytes(std::span(list)));
}

// a layout compiled in worker processes (see WorkerCount; docs/shader-compile-plan.md, steps 1 and 2): round one keys
// every entry point (each worker a share, the first reflecting the resources too: see shadercompiler::Action::kKeys);
// the entry points whose code the cache holds by their keys are taken from it; round two compiles the others (and for
// DXIL each specialization of the entry points that read the constant) in the workers at once, divided by what each
// took the last time (or by the size of its source), and puts them in the cache
static std::expected<rhi::ShaderSet, std::string> CompileInWorkers(
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

	// round one: the keys, and the reflection; or the cache's record of them for the sources as they are (see KeysRecord)
	const auto identity = KeysIdentity(file, config, request);
	size_t keyWorkerCount = 0;
	std::string keyTimings = " cached";
	auto record = ReadKeysRecord(cacheRoot, identity, entryCount);
	if (!record)
	{
		const auto roundStart = std::filesystem::file_time_type::clock::now();
		keyWorkerCount = std::clamp<size_t>(entryCount / kMinKeysPerWorker, 1, workerCount);
		std::vector<shadercompiler::Request> keyRequests(keyWorkerCount, request);
		for (size_t workerIt = 0; workerIt < keyWorkerCount; workerIt++)
		{
			keyRequests[workerIt].action = shadercompiler::Action::kKeys;
			keyRequests[workerIt].reflect = workerIt == 0;
			for (size_t entryPointIt = workerIt; entryPointIt < entryCount; entryPointIt += keyWorkerCount)
				keyRequests[workerIt].keyEntryPoints.push_back(static_cast<uint32_t>(entryPointIt));
		}
		auto keysRun = RunWorkers(keyRequests, jobs / "keys", file);
		if (!keysRun)
			return std::unexpected(keysRun.error());
		auto& keyResponses = *keysRun;
		auto& reflected = keyResponses[0];
		if (!reflected.diagnostics.empty())
		{
			std::print("{}", reflected.diagnostics);
			std::fflush(stdout);
		}
		record.emplace(KeysRecord{.dependencies = std::move(reflected.dependencies), .shaderSet = std::move(reflected.shaderSet), .keys = std::vector<shadercompiler::EntryKey>(entryCount)});
		for (const auto& response : keyResponses)
			for (const auto& key : response.keys)
				record->keys[key.entryPoint] = key;
		keyTimings = reflected.timings;
		WriteKeysRecord(cacheRoot, identity, *record, roundStart);
	}
	const auto keyed = std::chrono::steady_clock::now();
	const auto& keys = record->keys;

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
		auto codeRun = RunWorkers(codeRequests, jobs / "code", file);
		if (!codeRun)
			return std::unexpected(codeRun.error());
		auto& codeResponses = *codeRun;
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
	rhi::ShaderSet shaderSet = std::move(record->shaderSet);
	ENSUREF(shaderSet.shaders.size() == entryCount, "the reflection of {} has {} of {} entry points", file.string(), shaderSet.shaders.size(), entryCount);
	for (size_t entryPointIt = 0; entryPointIt < entryCount; entryPointIt++)
		std::get<0>(shaderSet.shaders[entryPointIt]) = std::move(*allJobs[entryPointIt].code);
	for (size_t jobIt = entryCount; jobIt < allJobs.size(); jobIt++)
		shaderSet.specializations.push_back({.shader = allJobs[jobIt].job.entryPoint, .value = allJobs[jobIt].job.value, .code = std::move(*allJobs[jobIt].code)});
	if (dependencies != nullptr)
		for (const auto& dependency : record->dependencies)
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
			keyTimings,
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
	// (where it compiles in this process: in worker processes, as many as compile at once)
	if (shaderloader::WorkerCount() == 0)
		ENSUREF(
			shaderloader::gLoaderCount.fetch_add(1, std::memory_order_acq_rel) == 0,
			"two ShaderLoaders alive at once: slang isn't thread safe, its users must take turns (see ShaderLoader)");
}

ShaderLoader::~ShaderLoader()
{
	if (shaderloader::WorkerCount() == 0)
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
	auto shaderSet = TryCompile(file, config, includePaths, intermediatePath, dependencies);
	// (a failure's diagnostics in the message: printed before it, to a piped stdout, they were lost when the assert
	// trapped before the buffer was flushed, a client's log in assettest)
	ENSUREF(shaderSet, "{}", shaderSet ? std::string() : shaderSet.error());
	return std::move(*shaderSet);
}

std::expected<rhi::ShaderSet, std::string> ShaderLoader::TryCompile(
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

	if (!response.ok)
		return std::unexpected(std::format("Failed to compile slang file {}:\n{}", file.string(), response.diagnostics));
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

std::vector<std::byte> ShaderLoader::SerializedResources(const rhi::ShaderSet& shaderSet)
{
	std::vector<std::byte> bytes;
	ENSUREF(!zpp::bits::failure(zpp::bits::out(bytes)(shaderSet.layouts)), "failed to serialize a shader set's resources");
	return bytes;
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
	auto shaderSet = TryLoad(file, config);
	ENSUREF(shaderSet, "{}", shaderSet ? std::string() : shaderSet.error());
	return std::move(*shaderSet);
}

std::expected<rhi::ShaderSet, std::string> ShaderLoader::TryLoad(const std::filesystem::path& file, const SlangConfiguration& config)
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
	std::string failure;
	auto compile = [this, &shaderSet, &dependencies, &file, &config, &failure](auto& /*todo: use me: in*/) -> std::error_code
	{
		auto compiled = TryCompile(file, config, myIncludePaths, myIntermediatePath, &dependencies);
		if (!compiled)
		{
			failure = compiled.error();
			return std::make_error_code(std::errc::invalid_argument);
		}
		shaderSet = std::move(*compiled);
		return {};
	};

	auto loadResult = core::file::LoadAsset(
		file, compile, loadBin, saveBin, ParametersHash(config), [&dependencies] { return dependencies; });

	if (!failure.empty())
		return std::unexpected(failure);
	ENSUREF(loadResult && !shaderSet.shaders.empty(), "Failed to load shaders.");

	return shaderSet;
}

} // namespace gfx::inline SPEEDO_GRAPHICS_API_NAMESPACE
