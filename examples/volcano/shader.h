#pragma once

#include "descriptorset.h"
#include "device.h"
#include "sampler.h"
#include "types.h"
#include "utils.h"

#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using ShaderBinary = std::vector<char>;

template <GraphicsBackend B>
using EntryPoint = std::tuple<std::string, ShaderStageFlagBits<B>>;

template <GraphicsBackend B>
using Shader = std::tuple<ShaderBinary, EntryPoint<B>>;

// this is intended to be a temporary object only used during loading.
template <GraphicsBackend B>
struct ShaderReflectionInfo
{
	std::vector<Shader<B>> shaders;
	std::map<uint32_t, DescriptorSetLayoutCreateDesc<B>> layouts;
};

namespace shader
{

template <GraphicsBackend B>
std::shared_ptr<ShaderReflectionInfo<B>> loadSlangShaders(
	const std::filesystem::path& slangFile,
	const std::vector<std::filesystem::path>& includePaths = {},
	const std::optional<std::filesystem::path>& compilerPath = std::nullopt,
	const std::optional<std::filesystem::path>& intermediatePath = std::nullopt);
	
}

template <GraphicsBackend B>
class ShaderModule : public DeviceObject<B>
{
public:

	constexpr ShaderModule() noexcept = default;
	ShaderModule(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const Shader<B>& shader);
	ShaderModule(ShaderModule&& other) noexcept;
    ~ShaderModule();

    ShaderModule& operator=(ShaderModule&& other) noexcept;
	operator auto() const { return myShaderModule; }

	void swap(ShaderModule& rhs) noexcept;
    friend void swap(ShaderModule& lhs, ShaderModule& rhs) noexcept { lhs.swap(rhs); }

	const auto& getEntryPoint() const { return myEntryPoint; }

private:

	ShaderModule( // takes ownership of provided handle
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        ShaderModuleHandle<B>&& shaderModule,
		const EntryPoint<B>& entryPoint);

	ShaderModuleHandle<B> myShaderModule = {};
	EntryPoint<B> myEntryPoint = {}; 
};

#include "shader.inl"
#include "shader-vulkan.inl"
