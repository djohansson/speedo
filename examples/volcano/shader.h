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

#include <slang.h>


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

class ShaderLoader
{
public:

	using DownstreamCompiler = std::tuple<SlangSourceLanguage, SlangPassThrough, std::filesystem::path>;

	ShaderLoader(
		std::vector<std::filesystem::path>&& includePaths,
		std::vector<DownstreamCompiler>&& downstreamCompilers,
		std::optional<std::filesystem::path>&& intermediatePath = std::nullopt)
		: myIncludePaths(std::forward<std::vector<std::filesystem::path>>(includePaths))
		, myDownstreamCompilers(std::forward<std::vector<DownstreamCompiler>>(downstreamCompilers))
		, myIntermediatePath(std::forward<std::optional<std::filesystem::path>>(intermediatePath))
		, myCompilerSession(spCreateSession(), spDestroySession)
	{
		for (const auto& [sourceLanguage, compilerId, compilerPath] : myDownstreamCompilers)
		{
			auto path = std::filesystem::canonical(compilerPath);

			std::cout << "Set downstream compiler path: " << path << std::endl;
			assert(std::filesystem::is_directory(path));
			
			myCompilerSession->setDownstreamCompilerPath(compilerId, path.generic_string().c_str());
			myCompilerSession->setDefaultDownstreamCompiler(sourceLanguage, compilerId);
		}
	}

	template <GraphicsBackend B>
	std::shared_ptr<ShaderReflectionInfo<B>> load(const std::filesystem::path& slangFile);

private:
	std::vector<std::filesystem::path> myIncludePaths;
	std::vector<DownstreamCompiler> myDownstreamCompilers;
	std::optional<std::filesystem::path> myIntermediatePath;
	std::unique_ptr<SlangSession, void(*)(SlangSession*)> myCompilerSession;
};

template <GraphicsBackend B>
class ShaderModule : public DeviceObject<B>
{
public:
	constexpr ShaderModule() noexcept = default;
	ShaderModule(const std::shared_ptr<DeviceContext<B>>& deviceContext, const Shader<B>& shader);
	ShaderModule(ShaderModule&& other) noexcept;
	~ShaderModule();

	ShaderModule& operator=(ShaderModule&& other) noexcept;
	operator auto() const noexcept { return myShaderModule; }

	void swap(ShaderModule& rhs) noexcept;
	friend void swap(ShaderModule& lhs, ShaderModule& rhs) noexcept { lhs.swap(rhs); }

	const auto& getEntryPoint() const noexcept { return myEntryPoint; }

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
