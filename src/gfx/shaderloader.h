#pragma once

#include <gfx/graphicsapi.h>

#include <rhi/shaderset.h>

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

namespace gfx::inline SPEEDO_GRAPHICS_API_NAMESPACE
{

// the debug information the shaders are compiled with: all of it in the builds that profile (for gpu captures), none in
// release, the package's build, whose precompiled main layout took 81 MB a variant with it
inline constexpr SlangDebugInfoLevel kShaderDebugInfoLevel =
	SPEEDO_PROFILING_LEVEL > 0 ? SLANG_DEBUG_INFO_LEVEL_MAXIMAL : SLANG_DEBUG_INFO_LEVEL_NONE;

// slang shaders for the copy's graphics api (gfx::kShaderFormat), with the resources they use: an install's precompiled
// (beside where their source would be, <stem>-<parameters hash>.shaderset, made when it was packaged: see
// tools/shaderprecompile.cpp), else compiled through the shader compiler library (gfx/shadercompiler.h: slang, loaded at
// the first compile, beside the executable) and cached as assets (see core::file::LoadAsset), recompiled when the source
// or an include changes. the client links no slang: a package ships neither it nor the compiler
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
		// the copy's backend's (gfx::kShaderFormat), unless asked otherwise (rhitest compiles for each backend)
		rhi::ShaderFormat format = kShaderFormat;

		[[nodiscard]] std::string ToString() const;
	};

	// one at a time in the process (a hard failure if another is alive): slang isn't thread safe, even across global
	// sessions, which share state (e.g. the embedded SPIR-V grammar, a static reference counted without atomics: sessions
	// made and destroyed on several threads freed it, and a later ~Session crashed). its users take turns by design (the
	// scene's compiles in the background, after the ImGui renderer's; the preview's once the window's scene is ready)
	constexpr ShaderLoader() noexcept = delete;
	explicit ShaderLoader(
		std::vector<std::filesystem::path>&& includePaths, std::optional<std::filesystem::path>&& intermediatePath = std::nullopt);
	~ShaderLoader();
	ShaderLoader(const ShaderLoader&) = delete;
	ShaderLoader(ShaderLoader&&) noexcept = delete;

	ShaderLoader& operator=(const ShaderLoader&) = delete;
	ShaderLoader& operator=(ShaderLoader&&) noexcept = delete;

	[[nodiscard]] rhi::ShaderSet Load(const std::filesystem::path& file, const SlangConfiguration& config);

	// what identifies a compile of a file: slang's version, the shader format and the configuration (a hash, hex)
	[[nodiscard]] static std::string ParametersHash(const SlangConfiguration& config);
	// where an install's precompiled shaders of file and config are (see Load)
	[[nodiscard]] static std::filesystem::path PrecompiledPath(const std::filesystem::path& file, const SlangConfiguration& config);
	// compiles file through the shader compiler library (a hard failure if it isn't there, or the compile fails), with the
	// include paths and where slang dumps its intermediates; the files it included in dependencies
	[[nodiscard]] static rhi::ShaderSet Compile(
		const std::filesystem::path& file,
		const SlangConfiguration& config,
		const std::vector<std::filesystem::path>& includePaths,
		const std::optional<std::filesystem::path>& intermediatePath,
		std::vector<std::filesystem::path>* dependencies = nullptr);
	// writes shaders where a precompiled set is read from (see tools/shaderprecompile.cpp)
	static void SavePrecompiled(const std::filesystem::path& path, const rhi::ShaderSet& shaderSet);

private:
	std::vector<std::filesystem::path> myIncludePaths;
	std::optional<std::filesystem::path> myIntermediatePath;
};

} // namespace gfx::inline SPEEDO_GRAPHICS_API_NAMESPACE
