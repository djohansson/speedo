#include "shader.h"

#include <iostream>

ShaderLoader::ShaderLoader(
	std::vector<std::filesystem::path>&& includePaths,
	std::vector<DownstreamCompiler>&& downstreamCompilers,
	std::optional<std::filesystem::path>&& intermediatePath)
	: myIncludePaths(std::forward<std::vector<std::filesystem::path>>(includePaths))
	, myDownstreamCompilers(std::forward<std::vector<DownstreamCompiler>>(downstreamCompilers))
	, myIntermediatePath(std::forward<std::optional<std::filesystem::path>>(intermediatePath))
	, myCompilerSession(spCreateSession(), spDestroySession)
{
	for (const auto& [sourceLanguage, compilerId, compilerPath] : myDownstreamCompilers)
	{
		myCompilerSession->setDefaultDownstreamCompiler(sourceLanguage, compilerId);

		if (!compilerPath || compilerPath->empty())
			continue;

		auto path = std::filesystem::canonical(compilerPath.value());
		
		std::cout << "Set downstream compiler path: " << path << '\n';
		ASSERT(std::filesystem::is_directory(path));

		myCompilerSession->setDownstreamCompilerPath(compilerId, path.generic_string().c_str());
	}
}

std::string ShaderLoader::SlangConfiguration::ToString() const
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
