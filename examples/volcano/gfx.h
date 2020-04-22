#pragma once

#include "buffer.h"
#include "device.h"
#include "file.h"
#include "gfx-types.h"
#include "glm.h"
#include "model.h"
#include "rendertarget.h"
#include "texture.h"
#include "utils.h"

#include <filesystem>
#include <map>
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
struct PipelineCacheHeader
{
};

template <GraphicsBackend B>
struct GraphicsPipelineResourceView
{	
	// begin temp
	std::shared_ptr<Model<B>> model;
	std::shared_ptr<Texture<B>> texture;
	ImageViewHandle<B> textureView = 0;
	SamplerHandle<B> sampler = 0;
	// end temp
	const RenderTarget<B>* renderTarget = nullptr;
};

template <GraphicsBackend B>
struct PipelineConfiguration
{
	std::shared_ptr<GraphicsPipelineResourceView<B>> resources;
	std::shared_ptr<PipelineLayoutContext<B>> layout;

	PipelineHandle<B> graphicsPipeline = 0; // ~ "PSO"

	std::vector<DescriptorSetHandle<B>> descriptorSets;
};

template <GraphicsBackend B>
bool isCacheValid(const PipelineCacheHeader<B>& header, const PhysicalDeviceProperties<B>& physicalDeviceProperties);

template <GraphicsBackend B>
std::tuple<SwapchainConfiguration<B>, std::optional<uint32_t>, PhysicalDeviceProperties<B>>
getSuitableSwapchainAndQueueFamilyIndex(SurfaceHandle<B> surface, PhysicalDeviceHandle<B> device);

template <GraphicsBackend B>
std::shared_ptr<SerializableShaderReflectionModule<B>> loadSlangShaders(const std::filesystem::path& slangFile);

template <GraphicsBackend B>
PipelineLayoutContext<B> createPipelineLayoutContext(DeviceHandle<B> device, const SerializableShaderReflectionModule<B>& slangModule);

template <GraphicsBackend B>
PipelineCacheHandle<B> loadPipelineCache(
	const std::filesystem::path& cacheFilePath,
	DeviceHandle<B> device,
	PhysicalDeviceProperties<B> physicalDeviceProperties);

template <GraphicsBackend B>
void savePipelineCache(
	const std::filesystem::path& cacheFilePath,
	DeviceHandle<B> device,
	PhysicalDeviceProperties<B> physicalDeviceProperties,
	PipelineCacheHandle<B> pipelineCache);

template <GraphicsBackend B>
PipelineCacheHandle<B> createPipelineCache(DeviceHandle<B> device, const std::vector<std::byte>& cacheData);

template <GraphicsBackend B>
std::vector<std::byte> getPipelineCacheData(DeviceHandle<B> device,	PipelineCacheHandle<B> pipelineCache);

template <GraphicsBackend B>
AllocatorHandle<B> createAllocator(
	InstanceHandle<B> instance,
	DeviceHandle<B> device,
	PhysicalDeviceHandle<B> physicalDevice);

template <GraphicsBackend B>
DescriptorPoolHandle<B> createDescriptorPool(DeviceHandle<B> device);

template <GraphicsBackend B>
PipelineHandle<B> createGraphicsPipeline(
	DeviceHandle<B> device,
	PipelineCacheHandle<B> pipelineCache,
	const PipelineConfiguration<B>& pipelineConfig);

#include "gfx.inl"
