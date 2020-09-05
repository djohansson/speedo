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

using ShaderBinary = std::vector<std::byte>;

template <GraphicsBackend B>
using EntryPoint = std::pair<std::string, ShaderStageFlagBits<B>>;

template <GraphicsBackend B>
using Shader = std::pair<ShaderBinary, EntryPoint<B>>;

// this is supposed to be a temporary object only used during loading.
template <GraphicsBackend B>
struct SerializableShaderReflectionInfo
{
	std::vector<Shader<B>> shaders;
	DescriptorSetLayoutBindingsMap<B> bindings;
	std::vector<SamplerCreateInfo<B>> immutableSamplers;
};

template <GraphicsBackend B>
class ShaderModule : public DeviceResource<B>
{
public:

    ShaderModule(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const Shader<B>& shader);
	ShaderModule( // takes ownership of provided handle
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        ShaderModuleHandle<B>&& shaderModule,
		const EntryPoint<B>& entryPoint);
	ShaderModule(ShaderModule&& other);
    ~ShaderModule();

    ShaderModule& operator=(ShaderModule&& other);
	operator auto() const { return myShaderModule; }

	const auto& getEntryPoint() const { return myEntryPoint; }

private:

	ShaderModuleHandle<B> myShaderModule = {};
	EntryPoint<B> myEntryPoint = {}; 
};

template <GraphicsBackend B>
std::shared_ptr<SerializableShaderReflectionInfo<B>> loadSlangShaders(
	const std::filesystem::path& compilerPath,
	const std::filesystem::path& slangFile);

#include "shader.inl"
