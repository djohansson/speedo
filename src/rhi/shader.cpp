#include "shader.h"

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
		auto path = std::filesystem::canonical(compilerPath);

		std::cout << "Set downstream compiler path: " << path << '\n';
		assert(std::filesystem::is_directory(path));

		myCompilerSession->setDownstreamCompilerPath(compilerId, path.generic_string().c_str());
		myCompilerSession->setDefaultDownstreamCompiler(sourceLanguage, compilerId);
	}
}
