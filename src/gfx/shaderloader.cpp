#include <core/assert.h>
#include <core/dynamiclibrary.h>
#include <core/file.h>
#include <core/path.h>
#include <gfx/shadercompiler.h>
#include <gfx/shaderloader.h>
#include <rhi/rhi.h>

#include <picosha2.h>

#include <array>
#include <atomic>
#include <format>
#include <print>
#include <span>
#include <utility>

namespace gfx
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

	return std::format(
		"sourceLanguage: {}, entryPoints: {}, "
		"optimizationLevel: {}, debugInfoLevel: {}, debugInfoFormat: {}, matrixLayoutMode: {}, definitions: {}",
		static_cast<SlangSourceLanguageIntegral>(sourceLanguage),
		entryPointsString,
		static_cast<SlangOptimizationLevelIntegral>(optimizationLevel),
		static_cast<SlangDebugInfoLevelIntegral>(debugInfoLevel),
		static_cast<SlangDebugInfoFormatIntegral>(debugInfoFormat),
		static_cast<SlangMatrixLayoutModeIntegral>(matrixLayoutMode),
		definitionsString);
}

std::string ShaderLoader::ParametersHash(const SlangConfiguration& config)
{
	std::string params;
	params.append("slang-");
	params.append(SPEEDO_SLANG_VERSION); // slang's version (its port's), so upgrading it recompiles
	params.append("|cache-v7"); // bump when the serialized ShaderSet layout changes, to invalidate stale caches
	params.append(std::format("|format-{}", std::to_underlying(rhi::kShaderFormat)));
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
	const auto& compiler = shaderloader::GetCompiler();
	ENSUREF(
		compiler.compile != nullptr,
		"{} needs compiling ({} isn't there: an install's shaders come precompiled, and this one isn't), and the shader "
		"compiler couldn't be loaded: {}",
		file.string(),
		PrecompiledPath(file, config).filename().string(),
		compiler.error);

	shadercompiler::Request request{
		.format = rhi::kShaderFormat,
		.file = file.generic_string(),
		.intermediatePath = intermediatePath ? intermediatePath->generic_string() : std::string{},
		.sourceLanguage = static_cast<int32_t>(config.sourceLanguage),
		.optimizationLevel = static_cast<int32_t>(config.optimizationLevel),
		.debugInfoLevel = static_cast<int32_t>(config.debugInfoLevel),
		.debugInfoFormat = static_cast<int32_t>(config.debugInfoFormat),
		.matrixLayoutMode = static_cast<int32_t>(config.matrixLayoutMode),
		.definitions = config.preprocessorDefinitions};
	for (const auto& includePath : includePaths)
		request.includePaths.push_back(includePath.generic_string());
	for (const auto& [name, stage] : config.entryPoints)
		request.entryPoints.push_back({.name = name, .stage = static_cast<int32_t>(stage)});

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

	if (!response.diagnostics.empty())
		std::print("{}", response.diagnostics);
	ENSUREF(response.ok, "Failed to compile slang file {}.", file.string());

	if (dependencies != nullptr)
		for (const auto& dependency : response.dependencies)
		{
			// recorded in the asset manifest, so editing an included/imported file recompiles. todo: hot reload
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

} // namespace gfx
