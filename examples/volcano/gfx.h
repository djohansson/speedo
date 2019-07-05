#pragma once

#include "gfx-types.h"
#include "glm.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

template <GraphicsBackend B>
struct PipelineLayoutContext
{
	std::unique_ptr<ShaderModuleHandle<B>[], ArrayDeleter<ShaderModuleHandle<B>>> shaders;
	std::unique_ptr<DescriptorSetLayoutHandle<B>[], ArrayDeleter<DescriptorSetLayoutHandle<B>>>
		descriptorSetLayouts;

	PipelineLayoutHandle<B> layout = 0;
};

template <GraphicsBackend B>
struct SwapchainInfo
{
	SurfaceCapabilities<B> capabilities = {};
	std::vector<SurfaceFormat<B>> formats;
	std::vector<PresentMode<B>> presentModes;
};

template <GraphicsBackend B>
struct SwapchainContext
{
	SwapchainInfo<B> info = {};
	SwapchainHandle<B> swapchain = 0;

	std::vector<ImageHandle<B>> colorImages;
	std::vector<ImageViewHandle<B>> colorImageViews;
};

using EntryPoint = std::pair<std::string, uint32_t>;
using ShaderBinary = std::vector<std::byte>;
using ShaderEntry = std::pair<ShaderBinary, EntryPoint>;

template <GraphicsBackend B>
struct SerializableDescriptorSetLayoutBinding : public DescriptorSetLayoutBinding<B>
{
	using BaseType = DescriptorSetLayoutBinding<B>;

	template <class Archive, GraphicsBackend B = B>
	typename std::enable_if_t<B == GraphicsBackend::Vulkan, void> serialize(Archive& ar)
	{
		static_assert(sizeof(*this) == sizeof(BaseType));

		ar(BaseType::binding);
		ar(BaseType::descriptorType);
		ar(BaseType::descriptorCount);
		ar(BaseType::stageFlags);
		// ar(pImmutableSamplers); // todo
	}
};

// this is a temporary object only used during loading.
template <GraphicsBackend B>
struct SerializableShaderReflectionModule
{
	using ShaderEntryVector = std::vector<ShaderEntry>;
	using BindingsMap =
		std::map<uint32_t, std::vector<SerializableDescriptorSetLayoutBinding<B>>>; // set, bindings

	template <class Archive>
	void serialize(Archive& ar)
	{
		ar(shaders);
		ar(bindings);
	}

	ShaderEntryVector shaders;
	BindingsMap bindings;
};

template <GraphicsBackend B>
struct SerializableVertexInputAttributeDescription : public VertexInputAttributeDescription<B>
{
	using BaseType = VertexInputAttributeDescription<B>;

	template <class Archive, GraphicsBackend B = B>
	typename std::enable_if_t<B == GraphicsBackend::Vulkan, void> serialize(Archive& ar)
	{
		static_assert(sizeof(*this) == sizeof(BaseType));

		ar(BaseType::location);
		ar(BaseType::binding);
		ar(BaseType::format);
		ar(BaseType::offset);
	}
};

template <GraphicsBackend B>
struct PipelineCacheHeader
{
};

struct ViewData
{
	struct ViewportData
	{
		uint32_t width = 0;
		uint32_t height = 0;
	} viewport;

	glm::vec3 camPos = glm::vec3(0.0f, -2.0f, 0.0f);
	glm::vec3 camRot = glm::vec3(0.0f, 0.0f, 0.0);

	glm::mat4x3 view = glm::mat4x3(1.0f);
	glm::mat4 projection = glm::mat4(1.0f);
};

template <GraphicsBackend B>
struct FrameData
{
	uint32_t index = 0;
	
	FramebufferHandle<B> frameBuffer = 0;
	std::vector<CommandBufferHandle<B>> commandBuffers; // count = [threadCount]
	
	FenceHandle<B> fence = 0;
	SemaphoreHandle<B> renderCompleteSemaphore = 0;
	SemaphoreHandle<B> newImageAcquiredSemaphore = 0;

	std::chrono::high_resolution_clock::time_point graphicsFrameTimestamp;
	std::chrono::duration<double> graphicsDeltaTime;
};

template <GraphicsBackend B>
bool isCacheValid(const PipelineCacheHeader<B>& header, const PhysicalDeviceProperties<B>& physicalDeviceProperties);

template <GraphicsBackend B>
std::tuple<SwapchainInfo<B>, int, PhysicalDeviceProperties<B>>
getSuitableSwapchainAndQueueFamilyIndex(SurfaceHandle<B> surface, PhysicalDeviceHandle<B> device);

template <GraphicsBackend B>
std::shared_ptr<SerializableShaderReflectionModule<B>> loadSlangShaders(const std::filesystem::path& slangFile);

template <GraphicsBackend B>
PipelineLayoutContext<B> createPipelineLayoutContext(DeviceHandle<B> device, const SerializableShaderReflectionModule<B>& slangModule);

template <GraphicsBackend B>
PipelineCacheHandle<B> loadPipelineCache(DeviceHandle<B> device, PhysicalDeviceProperties<B> physicalDeviceProperties,
	const std::filesystem::path& cacheFilePath);

template <GraphicsBackend B>
void savePipelineCache(DeviceHandle<B> device, PipelineCacheHandle<B> pipelineCache, PhysicalDeviceProperties<B> physicalDeviceProperties,
	const std::filesystem::path& cacheFilePath);

template <GraphicsBackend B>
InstanceHandle<B> createInstance();

#include "gfx.inl"
