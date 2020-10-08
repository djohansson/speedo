#pragma once

#include "descriptorset.h"
#include "device.h"
#include "file.h"
#include "image.h"
#include "model.h"
#include "rendertarget.h"
#include "shader.h"
#include "types.h"

#include <memory>
#include <optional>
#include <set>

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
        const std::shared_ptr<ShaderReflectionInfo<B>>& shaderModule);
    PipelineLayout( // takes ownership over provided handles
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        std::vector<ShaderModule<B>>&& shaderModules,
        std::vector<DescriptorSetLayout<B>>&& descriptorSetLayouts);
    PipelineLayout( // takes ownership over provided handles
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        std::vector<ShaderModule<B>>&& shaderModules,
        std::vector<DescriptorSetLayout<B>>&& descriptorSetLayouts,
        PipelineLayoutHandle<B>&& layout);
    ~PipelineLayout();

    PipelineLayout& operator=(PipelineLayout&& other);
    operator auto() const { return myLayout; }
    bool operator==(const PipelineLayoutHandle<B>& other) { return myLayout == other; }
    bool operator<(const PipelineLayout& other) { return myLayout < other; }

    const auto& getDescriptorSetLayouts() const { return myDescriptorSetLayouts; }
    const auto& getShaders() const { return myShaders; }
    uint64_t getHash() const { return 0; } // todo

private:

    std::vector<ShaderModule<B>> myShaders;
	std::vector<DescriptorSetLayout<B>> myDescriptorSetLayouts;
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

    const auto& getDesc() const { return myDesc; }
    auto getCache() const { return myCache; }
    auto getDescriptorPool() const { return myDescriptorPool; }

    const PipelineLayout<B>& getCurrentLayout() const { return *myCurrentLayout.value_or(myLayouts.cend()); }
    const PipelineLayout<B>& getLayout(PipelineLayoutHandle<B> handle) const { return *myLayouts.find(handle); }

    void setCurrentLayout(PipelineLayoutHandle<B> handle);
    
    PipelineLayoutHandle<B> emplaceLayout(PipelineLayout<B>&& layout) { return *myLayouts.emplace(std::move(layout)).first; }
    
    // temp! remove lazy updates and recalc when touched.
    // probably do not have these as shared ptrs, since we want pipeline to own them alone.
    const auto& resources() const { return myResources; }
    const auto& descriptorSets() const { return myDescriptorSets; }
    //

private:

    using PipelineLayoutSet = std::set<PipelineLayout<B>, std::less<>>;
    using PipelineLayoutConstIterator = typename PipelineLayoutSet::const_iterator;

    // todo:: move pipeline cache to its own class, and pass in reference to it.
    using PipelineMap = typename std::map<uint64_t, PipelineHandle<B>>;
    using PipelineMapConstIterator = typename PipelineMap::const_iterator;

    uint64_t internalCalculateHashKey() const;
    PipelineHandle<B> internalCreateGraphicsPipeline(uint64_t hashKey);
    PipelineMapConstIterator internalUpdateMap();

    const PipelineCreateDesc<B> myDesc = {};
    PipelineLayoutSet myLayouts;
    PipelineCacheHandle<B> myCache = {};
    PipelineMap myPipelineMap;
    DescriptorPoolHandle<B> myDescriptorPool = {};

    // temp
    std::shared_ptr<PipelineResourceView<B>> myResources;
	std::shared_ptr<DescriptorSetVector<B>> myDescriptorSets;
    //

    // shadow state
    std::vector<PipelineShaderStageCreateInfo<B>> myShaderStages;
    PipelineVertexInputStateCreateInfo<B> myVertexInput = {};
    PipelineInputAssemblyStateCreateInfo<B> myInputAssembly = {};
    std::vector<Viewport<B>> myViewports;
    std::vector<Rect2D<B>> myScissorRects;
    PipelineViewportStateCreateInfo<B> myViewport = {};
    PipelineRasterizationStateCreateInfo<B> myRasterization = {};
    PipelineMultisampleStateCreateInfo<B> myMultisample = {};
    PipelineDepthStencilStateCreateInfo<B> myDepthStencil = {};
    std::vector<PipelineColorBlendAttachmentState<B>> myColorBlendAttachments = {};
    PipelineColorBlendStateCreateInfo<B> myColorBlend = {};
    std::vector<DynamicState<B>> myDynamicStateDescs;
    PipelineDynamicStateCreateInfo<B> myDynamicState = {};
    std::tuple<RenderPassHandle<Vk>, FramebufferHandle<Vk>> myRenderPassAndFramebuffer;
    uint32_t mySubpass = 0;
    std::optional<PipelineLayoutConstIterator> myCurrentLayout;
    //
    
    std::unique_ptr<XXH3_state_t, XXH_errorcode(*)(XXH3_state_t*)> myXXHState = { XXH3_createState(), XXH3_freeState };
};
