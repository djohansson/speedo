// shaderprecompile <shader dir> <out dir>: compiles every shader layout an installed client loads (gfx/shaderlayouts.h:
// the ImGui renderer's and the scene's, with and without the device's ray queries) into <out dir>, as the precompiled
// sets gfx::ShaderLoader::Load reads (<stem>-<parameters hash>.shaderset), the ones there before removed. scripts/package.ps1
// runs it: a package ships them in place of the shader sources, the shader compiler and slang. it compiles through the
// shader compiler library beside it, as the client does (see gfx/shadercompiler.h)
#include <gfx/shaderlayouts.h>
#include <gfx/shaderloader.h>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <print>
#include <vector>

int main(int argc, char** argv)
{
	if (argc != 3)
	{
		std::println(stderr, "usage: shaderprecompile <shader dir> <out dir>");
		return 2;
	}
	std::filesystem::path shaderPath = std::filesystem::absolute(argv[1]);
	std::filesystem::path outPath = std::filesystem::absolute(argv[2]);

	std::filesystem::create_directories(outPath);
	for (const auto& entry : std::filesystem::directory_iterator(outPath))
		if (entry.path().extension() == ".shaderset")
			std::filesystem::remove(entry.path());

	std::vector<gfx::ShaderLayout> layouts{gfx::ImGuiShaderLayout(shaderPath)};
	for (bool rayQuery : {false, true})
		std::ranges::move(gfx::SceneShaderLayouts(shaderPath, rayQuery, false), std::back_inserter(layouts));

	std::vector<std::filesystem::path> written;
	for (const auto& layout : layouts)
	{
		// the flares' and the environment filter's are the same either way
		auto name = gfx::ShaderLoader::PrecompiledPath(layout.file, layout.config).filename();
		if (std::ranges::contains(written, name))
			continue;
		auto shaderSet = gfx::ShaderLoader::Compile(layout.file, layout.config, {shaderPath}, std::nullopt);
		gfx::ShaderLoader::SavePrecompiled(outPath / name, shaderSet);
		written.push_back(name);
		std::println("{} ({} entry points)", (outPath / name).string(), shaderSet.shaders.size());
	}
	std::println("{} precompiled shader sets in {}", written.size(), outPath.string());
	return 0;
}
