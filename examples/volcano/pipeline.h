#pragma once

#include "descriptorset.h"
#include "device.h"
#include "file.h"
#include "image.h"
#include "model.h"
#include "rendertarget.h"
#include "shader.h"
#include "types.h"

#include <map>
#include <memory>
#include <optional>

template <GraphicsBackend B>
struct PipelineCacheHeader
{
};

template <GraphicsBackend B>
class PipelineLayout : public DeviceResource<B>
{
public:

    PipelineLayout(PipelineLayout<B>&& other) = default;
    PipelineLayout(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const std::shared_ptr<SerializableShaderReflectionModule<B>>& shaderModule);
    ~PipelineLayout();

    PipelineLayout& operator=(PipelineLayout&& other) = default;

    const auto& getDescriptorSetLayouts() const { return myDescriptorSetLayouts; }
    const auto& getShaders() const { return myShaders; }
    const auto& getImmutableSamplers() const { return myImmutableSamplers; }
    auto getLayout() const { return myLayout; }

private:

    PipelineLayout(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        DescriptorSetLayoutVector<B>&& descriptorSetLayouts);

    PipelineLayout(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        DescriptorSetLayoutVector<B>&& descriptorSetLayouts,
        PipelineLayoutHandle<B>&& layout);

	DescriptorSetLayoutVector<B> myDescriptorSetLayouts;
    std::unique_ptr<ShaderModuleHandle<B>[], ArrayDeleter<ShaderModuleHandle<B>>> myShaders;
    std::unique_ptr<SamplerHandle<B>[], ArrayDeleter<SamplerHandle<B>>> myImmutableSamplers;
	PipelineLayoutHandle<B> myLayout = 0;
};

template <GraphicsBackend B>
struct PipelineResourceView
{	
	// begin temp
	std::shared_ptr<Model<B>> model;
	std::shared_ptr<Image<B>> image;
	std::shared_ptr<ImageView<B>> imageView;
	SamplerHandle<B> sampler = 0;
	// end temp
	std::shared_ptr<RenderTarget<B>> renderTarget;
};

template <GraphicsBackend B>
struct PipelineContextCreateDesc : DeviceResourceCreateDesc<B>
{
    std::filesystem::path userProfilePath;
};

// todo: create maps with sensible hash keys for each structure that goes into vkCreateGraphicsPipelines()
//       combine them to get a compisite hash for the actual pipeline object

template <GraphicsBackend B>
class PipelineContext : public DeviceResource<B>
{
public:

    PipelineContext(PipelineContext<B>&& other) = default;
    PipelineContext(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        PipelineContextCreateDesc<B>&& desc);
    ~PipelineContext();

    PipelineContext& operator=(PipelineContext&& other) = default;

    auto getCache() const { return myCache; }
    auto getPipeline() const { return myCurrent.value()->second; }

    // temp!
    void updateDescriptorSets(BufferHandle<GraphicsBackend::Vulkan> buffer);
    void createGraphicsPipeline();
    auto& resources() { return myResources; }
    auto& layout() { return myLayout; }
    auto& descriptorSets() { return myDescriptorSets; }
    //

private:

    using PipelineMap = typename std::map<uint64_t, PipelineHandle<B>>;

    const PipelineContextCreateDesc<B> myDesc = {};
    PipelineCacheHandle<B> myCache = 0;
    PipelineMap myPipelineMap;
    std::optional<typename PipelineMap::const_iterator> myCurrent;

    // temp
    std::shared_ptr<PipelineResourceView<B>> myResources;
	std::shared_ptr<PipelineLayout<B>> myLayout;
	std::shared_ptr<DescriptorSetVector<B>> myDescriptorSets;
    //
};
