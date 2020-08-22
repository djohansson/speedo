#pragma once

#include "descriptorset.h"
#include "device.h"
#include "sampler.h"
#include "types.h"

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
class ShaderModule : public DeviceResource<B>
{
public:

    ShaderModule(ShaderModule&& other);
    ShaderModule(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const ShaderEntry& shaderEntry);
	ShaderModule( // takes ownership of provided handle
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        ShaderModuleHandle<B>&& shaderModule);
    ~ShaderModule();

    ShaderModule& operator=(ShaderModule&& other);
	operator auto() const { return myShaderModule; }

private:

	ShaderModuleHandle<B> myShaderModule = {};
};

template <GraphicsBackend B>
std::shared_ptr<SerializableShaderReflectionModule<B>> loadSlangShaders(
	const std::filesystem::path& compilerPath,
	const std::filesystem::path& slangFile);

template <GraphicsBackend B>
void createSlangLayoutBindings(slang::VariableLayoutReflection* parameter, DescriptorSetLayoutBindingsMap<B>& bindings);

#include "shader.inl"
