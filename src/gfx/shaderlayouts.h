#pragma once

#include <gfx/shaderloader.h>

#include <filesystem>
#include <vector>

// the shader layouts the client loads, each its source file and how it is compiled: what a package precompiles
// (tools/shaderprecompile.cpp), every variant a device may ask for, so that an installed client needs no shader compiler
namespace gfx::inline SPEEDO_GRAPHICS_API_NAMESPACE
{

struct ShaderLayout
{
	std::filesystem::path file;
	ShaderLoader::SlangConfiguration config;
};

// the ImGui renderer's (imguirenderer.cpp)
[[nodiscard]] ShaderLayout ImGuiShaderLayout(const std::filesystem::path& shaderPath);

// the scene's (scene.cpp): the pre-pass, the main layout, the flares' and the environment filter's, with the device's ray
// queries (BVH_RAY_QUERY) or without, and with the ray query watch (a diagnostic, which an install doesn't have)
[[nodiscard]] std::vector<ShaderLayout> SceneShaderLayouts(const std::filesystem::path& shaderPath, bool rayQuery, bool rayQueryWatch);

} // namespace gfx::inline SPEEDO_GRAPHICS_API_NAMESPACE
