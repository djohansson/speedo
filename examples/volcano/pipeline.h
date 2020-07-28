#pragma once

#include "descriptorset.h"
#include "device.h"
#include "file.h"
#include "image.h"
#include "model.h"
#include "rendertarget.h"
#include "shader.h"
#include "types.h"

template <GraphicsBackend B>
struct PipelineCacheHeader
{
};

template <GraphicsBackend B>
class PipelineLayoutContext : public DeviceResource<B>
{
public:

    PipelineLayoutContext(PipelineLayoutContext<B>&& other) = default;
    PipelineLayoutContext(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const std::shared_ptr<SerializableShaderReflectionModule<B>>& shaderModule);
    ~PipelineLayoutContext();

    const auto& getDescriptorSetLayouts() const { return myDescriptorSetLayouts; }
    const auto& getShaders() const { return myShaders; }
    const auto& getImmutableSamplers() const { return myImmutableSamplers; }
    auto getLayout() const { return myLayout; }

private:

    PipelineLayoutContext(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        DescriptorSetLayoutVector<B>&& descriptorSetLayouts);

    PipelineLayoutContext(
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
struct PipelineConfiguration
{
	std::shared_ptr<PipelineResourceView<B>> resources;
	std::shared_ptr<PipelineLayoutContext<B>> layout;
	std::shared_ptr<DescriptorSetVector<B>> descriptorSets;

    // temp - this should perhaps be an iterator into PipelineContext:s map
	PipelineHandle<B> graphicsPipeline = 0;
    //
};

template <GraphicsBackend B>
struct PipelineContextCreateDesc : DeviceResourceCreateDesc<B>
{
};

// todo: create maps with sensible hash keys for each structure that goes into vkCreateGraphicsPipelines()
//       combine them to get a compisite hash for the actual pipeline object

template <GraphicsBackend B>
class PipelineContext : public DeviceResource<B>
{
public:

    PipelineContext(PipelineContext<B>&& other) = default;
    PipelineContext(
        const std::shared_ptr<InstanceContext<B>>& instanceContext,
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const std::shared_ptr<SerializableShaderReflectionModule<GraphicsBackend::Vulkan>>& shaderModule,
        const std::filesystem::path& userProfilePath,
        PipelineContextCreateDesc<B>&& desc);
    ~PipelineContext();

    const auto& getConfig() const { return myConfig; }
    const auto getCache() const { return myCache; }

    // temp!
    void updateDescriptorSets(BufferHandle<GraphicsBackend::Vulkan> buffer) const;
    void createGraphicsPipeline();
    //

private:

    using PipelineMap = typename std::map<uint64_t, PipelineHandle<B>>;

    const PipelineContextCreateDesc<B> myDesc = {};
    PipelineCacheHandle<B> myCache = 0;
    PipelineMap myPipelineMap;
    std::optional<typename PipelineMap::const_iterator> myCurrent;


    // temp crap
    std::shared_ptr<InstanceContext<B>> myInstance;
    std::shared_ptr<PipelineConfiguration<B>> myConfig;
    std::filesystem::path myUserProfilePath;
    //
};
