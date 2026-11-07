#pragma once

#include <rhi/shaderset.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#ifdef None // None is defined in X11/X.h
#undef None
#endif
#ifdef Bool // Bool is defined in X11/X.h
#undef Bool
#endif
#include <slang.h>

namespace gfx
{

// compiles slang shaders for the build's graphics api (rhi::kShaderFormat), and reflects the resources they use.
// results are cached as assets (see core::file::LoadAsset), recompiled when the source or an include changes.
class ShaderLoader final
{
public:
	struct SlangConfiguration
	{
		SlangSourceLanguage sourceLanguage = SLANG_SOURCE_LANGUAGE_UNKNOWN;
		std::vector<std::pair<std::string, SlangStage>> entryPoints;
		SlangOptimizationLevel optimizationLevel = SLANG_OPTIMIZATION_LEVEL_DEFAULT;
		SlangDebugInfoLevel debugInfoLevel = SLANG_DEBUG_INFO_LEVEL_STANDARD;
		SlangDebugInfoFormat debugInfoFormat = SLANG_DEBUG_INFO_FORMAT_DEFAULT;
		SlangMatrixLayoutMode matrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;
		std::vector<std::pair<std::string, std::string>> preprocessorDefinitions = { {"SHADERTYPES_H_GPU_TARGET", "true"} };

		[[nodiscard]] std::string ToString() const;
	};

	using DownstreamCompiler =
		std::tuple<SlangSourceLanguage, SlangPassThrough, std::optional<std::filesystem::path>>;

	constexpr ShaderLoader() noexcept = delete;
	ShaderLoader(
		std::vector<std::filesystem::path>&& includePaths,
		std::vector<DownstreamCompiler>&& downstreamCompilers,
		std::optional<std::filesystem::path>&& intermediatePath = std::nullopt);
	ShaderLoader(const ShaderLoader&) = delete;
	ShaderLoader(ShaderLoader&&) noexcept = delete;

	ShaderLoader& operator=(const ShaderLoader&) = delete;
	ShaderLoader& operator=(ShaderLoader&&) noexcept = delete;

	// the shaders of config's entry points in file, from the cache (see core::file::LoadAsset) or compiled: in groups of
	// its entry points at once (GroupCount, each a compile request of its own slang session, on the application's
	// executor). each group parses and checks the whole source (the main layout's: 0.55 s of its 22.7 s on one, M4 Pro)
	// and generates the code of its entry points, what they share again in each: 4 groups took the scene's cold first load
	// from 28.8 to 18.0 s, its resident peak from 850 MB to 1.8 GB (8: 17.0 s, 2.5 GB). each group done adds one to
	// groupsDone (if given), and a cached load all of them at once
	[[nodiscard]] rhi::ShaderSet Load(
		const std::filesystem::path& file, const SlangConfiguration& config, std::atomic_uint32_t* groupsDone = nullptr);

	// how many groups config's entry points compile in (see Load): one under kGroupEntryPointCount of them, else up to the
	// executor's threads and kMaxGroupCount (SPEEDO_SHADER_COMPILE_GROUPS overrides it)
	[[nodiscard]] static uint32_t GroupCount(const SlangConfiguration& config);
	static constexpr uint32_t kGroupEntryPointCount = 8;
	static constexpr uint32_t kMaxGroupCount = 4;

private:
	// a slang session of its own, with the downstream compilers set (see the constructor)
	[[nodiscard]] std::unique_ptr<SlangSession, void (*)(SlangSession*)> InternalCreateSession() const;

	std::vector<std::filesystem::path> myIncludePaths;
	std::vector<DownstreamCompiler> myDownstreamCompilers;
	std::optional<std::filesystem::path> myIntermediatePath;
	std::unique_ptr<SlangSession, void (*)(SlangSession*)> myCompilerSession;
};

} // namespace gfx
