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

#include <slang.h>

using ShaderBinary = std::vector<char>;

template <GraphicsApi G>
using EntryPoint = std::tuple<std::string, ShaderStageFlagBits<G>>;

template <GraphicsApi G>
using Shader = std::tuple<ShaderBinary, EntryPoint<G>>;

template <GraphicsApi G>
struct ShaderSet
{
	std::vector<Shader<G>> shaders;
	std::map<uint32_t, DescriptorSetLayoutCreateDesc<G>> layouts;
};

class ShaderLoader final : public Noncopyable, public Nonmovable
{
public:
	using DownstreamCompiler =
		std::tuple<SlangSourceLanguage, SlangPassThrough, std::filesystem::path>;

	ShaderLoader(
		std::vector<std::filesystem::path>&& includePaths,
		std::vector<DownstreamCompiler>&& downstreamCompilers,
		std::optional<std::filesystem::path>&& intermediatePath = std::nullopt);

	template <GraphicsApi G>
	ShaderSet<G> load(const std::filesystem::path& slangFile);

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
	~ShaderModule();

	ShaderModule& operator=(ShaderModule&& other) noexcept;
	operator auto() const noexcept { return myShaderModule; }

	void Swap(ShaderModule& rhs) noexcept;
	friend void Swap(ShaderModule& lhs, ShaderModule& rhs) noexcept { lhs.Swap(rhs); }

	const auto& GetEntryPoint() const noexcept { return myEntryPoint; }

private:
	ShaderModule( // takes ownership of provided handle
		const std::shared_ptr<Device<G>>& device,
		ShaderModuleHandle<G>&& shaderModule,
		const EntryPoint<G>& entryPoint);

	ShaderModuleHandle<G> myShaderModule{};
	EntryPoint<G> myEntryPoint{};
};

#include "shader.inl"
