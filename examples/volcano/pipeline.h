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

#include <xxh3.h>

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
        ShaderModuleVector<B>&& shaderModules,
        DescriptorSetLayoutVector<B>&& descriptorSetLayouts);

    PipelineLayout(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        ShaderModuleVector<B>&& shaderModules,
        DescriptorSetLayoutVector<B>&& descriptorSetLayouts,
        PipelineLayoutHandle<B>&& layout);

    ShaderModuleVector<B> myShaders;
	DescriptorSetLayoutVector<B> myDescriptorSetLayouts;
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
    std::filesystem::path cachePath;
};

// todo: create maps with sensible hash keys for each structure that goes into vkCreateGraphicsPipelines()
//       combine them to get a compisite hash for the actual pipeline object (Merkle tree)

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
    PipelineHandle<B> getPipeline();

    // temp! remove lazy updates and recalc when touched.
    void updateDescriptorSets(BufferHandle<Vk> buffer);
    auto& resources() { return myResources; }
    auto& layout() { return myLayout; }
    auto& descriptorSets() { return myDescriptorSets; }
    //

private:

    using PipelineMap = typename std::map<uint64_t, PipelineHandle<B>>;

    uint64_t internalCalculateHashKey() const;
    PipelineHandle<B> internalCreateGraphicsPipeline(uint64_t hashKey);
    typename PipelineMap::const_iterator internalUpdateMap();

    const PipelineContextCreateDesc<B> myDesc = {};
    PipelineCacheHandle<B> myCache = 0;
    PipelineMap myPipelineMap;

    // temp
    std::shared_ptr<PipelineResourceView<B>> myResources;
	std::shared_ptr<PipelineLayout<B>> myLayout;
	std::shared_ptr<DescriptorSetVector<B>> myDescriptorSets;
    //

    std::shared_mutex myMutex; // todo: replace with asserting mutex
    
    std::unique_ptr<XXH3_state_t, XXH_errorcode(*)(XXH3_state_t*)> myXXHState = { XXH3_createState(), XXH3_freeState };
};
