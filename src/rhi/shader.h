#pragma once

#include "descriptorset.h"
#include "device.h"
#include "sampler.h"
#include "types.h"

#include <core/utils.h>

#include <map>
#include <memory>
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

using ShaderBinary = std::vector<char>;

struct ComputeLaunchParameters
{
	std::array<uint64_t, 3> threadGroupSize;
	uint64_t waveSize{};
};

template <GraphicsApi G>
using EntryPoint = std::tuple<std::string, ShaderStageFlagBits<G>, std::optional<ComputeLaunchParameters>>;

template <GraphicsApi G>
using Shader = std::tuple<ShaderBinary, EntryPoint<G>>;

template <GraphicsApi G>
struct ShaderSet
{
	std::vector<Shader<G>> shaders;
	std::map<uint32_t, DescriptorSetLayoutCreateDesc<G>> layouts;
};

// todo: make into an interface and move slang implementation to a separate file
class ShaderLoader final
{
public:
	struct SlangConfiguration
	{
		SlangSourceLanguage sourceLanguage = SLANG_SOURCE_LANGUAGE_UNKNOWN;
		SlangCompileTarget target = SLANG_TARGET_UNKNOWN;
		std::string targetProfile;
		std::vector<std::pair<std::string, SlangStage>> entryPoints;
		SlangOptimizationLevel optimizationLevel = SLANG_OPTIMIZATION_LEVEL_DEFAULT;
		SlangDebugInfoLevel debugInfoLevel = SLANG_DEBUG_INFO_LEVEL_STANDARD;
		SlangDebugInfoFormat debugInfoFormat = SLANG_DEBUG_INFO_FORMAT_DEFAULT;
		SlangMatrixLayoutMode matrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;

		std::string ToString() const
		{
			std::string entryPointsString;
			for (const auto& [name, stage] : entryPoints)
				entryPointsString.append(std::format("[{}, {}]", name, static_cast<SlangStageIntegral>(stage)));

			return std::format(
				"sourceLanguage: {}, target: {}, targetProfile: {}, entryPoints: {}, "
				"optimizationLevel: {}, debugInfoLevel: {}, debugInfoFormat: {}, matrixLayoutMode: {}",
				static_cast<SlangSourceLanguageIntegral>(sourceLanguage),
				static_cast<SlangCompileTargetIntegral>(target),
				targetProfile,
				entryPointsString,
				static_cast<SlangOptimizationLevelIntegral>(optimizationLevel),
				static_cast<SlangDebugInfoLevelIntegral>(debugInfoLevel),
				static_cast<SlangDebugInfoFormatIntegral>(debugInfoFormat),
				static_cast<SlangMatrixLayoutModeIntegral>(matrixLayoutMode));
		}
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

	template <GraphicsApi G>
	[[nodiscard]] ShaderSet<G> Load(const std::filesystem::path& file, const SlangConfiguration& config);

private:
	std::vector<std::filesystem::path> myIncludePaths;
	std::vector<DownstreamCompiler> myDownstreamCompilers;
	std::optional<std::filesystem::path> myIntermediatePath;
	std::unique_ptr<SlangSession, void (*)(SlangSession*)> myCompilerSession;
};

template <GraphicsApi G>
class ShaderModule final : public DeviceObject<G>
{
public:
	constexpr ShaderModule() noexcept = default;
	ShaderModule(const std::shared_ptr<Device<G>>& device, const Shader<G>& shader);
	ShaderModule(ShaderModule&& other) noexcept;
	~ShaderModule() override;

	ShaderModule& operator=(ShaderModule&& other) noexcept;
	operator auto() const noexcept { return myShaderModule; }//NOLINT(google-explicit-constructor)

	void Swap(ShaderModule& rhs) noexcept;
	friend void Swap(ShaderModule& lhs, ShaderModule& rhs) noexcept { lhs.Swap(rhs); }

	[[nodiscard]] const auto& GetEntryPoint() const noexcept { return myEntryPoint; }

private:
	ShaderModule( // takes ownership of provided handle
		const std::shared_ptr<Device<G>>& device,
		ShaderModuleHandle<G>&& shaderModule,
		const EntryPoint<G>& entryPoint);

	ShaderModuleHandle<G> myShaderModule{};
	EntryPoint<G> myEntryPoint{};
};

#include "shader.inl"
