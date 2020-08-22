#pragma once

#include "descriptorset.h"
#include "types.h"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <slang.h>

using EntryPoint = std::pair<std::string, uint32_t>;
using ShaderBinary = std::vector<std::byte>;
using ShaderEntry = std::pair<ShaderBinary, EntryPoint>;

// this is supposed to be a temporary object only used during loading.
template <GraphicsBackend B>
struct SerializableShaderReflectionModule
{
	std::vector<ShaderEntry> shaders;
	DescriptorSetLayoutBindingsMap<B> bindings;
	std::vector<SamplerCreateInfo<B>> immutableSamplers;
};

template <GraphicsBackend B>
class ShaderModuleVector : public DeviceResource<B>
{
public:

    ShaderModuleVector(ShaderModuleVector&& other) = default;
    ShaderModuleVector(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const std::vector<ShaderEntry>& shaderEntries);
    ~ShaderModuleVector();

    ShaderModuleVector& operator=(ShaderModuleVector&& other) = default;
	ShaderModuleHandle<B> operator[](uint32_t index) const { return myShaderModules[index]; };

private:

    ShaderModuleVector( // uses provided vector
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        std::vector<ShaderModuleHandle<B>>&& shaderModules);

	std::vector<ShaderModuleHandle<B>> myShaderModules;
};

template <GraphicsBackend B>
std::shared_ptr<SerializableShaderReflectionModule<B>> loadSlangShaders(
	const std::filesystem::path& compilerPath,
	const std::filesystem::path& slangFile);

template <GraphicsBackend B>
void createSlangLayoutBindings(slang::VariableLayoutReflection* parameter, DescriptorSetLayoutBindingsMap<B>& bindings);

#include "shader.inl"
#include "shader-vulkan.inl"
