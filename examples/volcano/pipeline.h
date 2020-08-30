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

    PipelineLayout(PipelineLayout<B>&& other);
    PipelineLayout(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const std::shared_ptr<SerializableShaderReflectionModule<B>>& shaderModule);
    PipelineLayout( // takes ownership over provided handles
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        std::vector<ShaderModule<B>>&& shaderModules,
        std::vector<DescriptorSetLayout<B>>&& descriptorSetLayouts,
        std::vector<Sampler<B>>&& immutableSamplers);
    PipelineLayout( // takes ownership over provided handles
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        std::vector<ShaderModule<B>>&& shaderModules,
        std::vector<DescriptorSetLayout<B>>&& descriptorSetLayouts,
        std::vector<Sampler<B>>&& immutableSamplers,
        PipelineLayoutHandle<B>&& layout);
    ~PipelineLayout();

    PipelineLayout& operator=(PipelineLayout&& other);
    operator auto() const { return myLayout; }

    const auto& getDescriptorSetLayouts() const { return myDescriptorSetLayouts; }
    const auto& getShaders() const { return myShaders; }
    const auto& getImmutableSamplers() const { return myImmutableSamplers; }
    auto getLayout() const { return myLayout; }

private:

    std::vector<ShaderModule<B>> myShaders;
	std::vector<DescriptorSetLayout<B>> myDescriptorSetLayouts;
    std::vector<Sampler<B>> myImmutableSamplers;
	PipelineLayoutHandle<B> myLayout = {};
};

template <GraphicsBackend B>
struct PipelineResourceView
{	
	// begin temp
	std::shared_ptr<Model<B>> model;
	std::shared_ptr<Image<B>> image;
	std::shared_ptr<ImageView<B>> imageView;
	SamplerHandle<B> sampler = {};
	// end temp
	std::shared_ptr<RenderTarget<B>> renderTarget;
};

template <GraphicsBackend B>
struct PipelineCreateDesc : DeviceResourceCreateDesc<B>
{
    std::filesystem::path cachePath;
};

// todo: create maps with sensible hash keys for each structure that goes into vkCreateGraphicsPipelines()
//       combine them to get a compisite hash for the actual pipeline object (Merkle tree)

template <GraphicsBackend B>
class Pipeline : public DeviceResource<B>
{
public:

    Pipeline(Pipeline<B>&& other);
    Pipeline(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        PipelineCreateDesc<B>&& desc);
    ~Pipeline();

    Pipeline& operator=(Pipeline&& other);
    operator auto() { return internalUpdateMap()->second; };

    auto getCache() const { return myCache; }

    // temp! remove lazy updates and recalc when touched.
    auto& resources() { return myResources; }
    auto& layout() { return myLayout; }
    auto& descriptorSets() { return myDescriptorSets; }
    //

private:

    using PipelineMap = typename std::map<uint64_t, PipelineHandle<B>>;

    uint64_t internalCalculateHashKey() const;
    PipelineHandle<B> internalCreateGraphicsPipeline(uint64_t hashKey);
    typename PipelineMap::const_iterator internalUpdateMap();

    const PipelineCreateDesc<B> myDesc = {};
    PipelineCacheHandle<B> myCache = {};
    PipelineMap myPipelineMap;

    // temp
    std::shared_ptr<PipelineResourceView<B>> myResources;
	std::shared_ptr<PipelineLayout<B>> myLayout;
	std::shared_ptr<DescriptorSetVector<B>> myDescriptorSets;
    //
    
    std::unique_ptr<XXH3_state_t, XXH_errorcode(*)(XXH3_state_t*)> myXXHState = { XXH3_createState(), XXH3_freeState };
};
