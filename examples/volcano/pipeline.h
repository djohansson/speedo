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

    const auto& getDescriptorSetLayouts() const { return myDescriptorSetLayouts; }
    const auto& getShaders() const { return myShaders; }

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
struct PipelineConfiguration : DeviceResourceCreateDesc<B>
{
    std::filesystem::path cachePath;
};

// todo: create maps with sensible hash keys for each structure that goes into vkCreateGraphicsPipelines()
//       combine them to get a compisite hash for the actual pipeline object (Merkle tree)

template <GraphicsBackend B>
class PipelineContext : public DeviceResource<B>
{
public:

    PipelineContext(PipelineContext<B>&& other);
    PipelineContext(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        AutoSaveJSONFileObject<PipelineConfiguration<B>>&& config);
    ~PipelineContext();

    PipelineContext& operator=(PipelineContext&& other);
    operator auto() { return internalUpdateMap()->second; };

    const auto& getConfig() const { return myConfig; }
    auto getCache() const { return myCache; }
    auto getDescriptorPool() const { return myDescriptorPool; }
    const PipelineLayout<B>& getLayout() const { return *myLayout; }
    const auto& getDescriptorSet(uint32_t set) const { return myDescriptorSets[set][0]; }

    void setLayout(PipelineLayoutHandle<B> handle);
    
    PipelineLayoutHandle<B> emplaceLayout(PipelineLayout<B>&& layout);

    template <typename T>
    void setDescriptor(T&& data, DescriptorType<B> type, uint32_t set, uint32_t binding, uint32_t index = 0);
    
    //void copyDescriptorSet(uint32_t set, DescriptorSetArray<B>& dst) const;
    void pushDescriptorSet(
        CommandBufferHandle<B> cmd,
        PipelineBindPoint<B> bindPoint,
        PipelineLayoutHandle<B> layout,
        uint32_t set) const;
    void writeDescriptorSet(uint32_t set) const;
    
    // temp! remove lazy updates and recalc when touched.
    // probably do not have these as shared ptrs, since we likely want pipeline to own them.
    const auto& resources() const { return myResources; }
    //

private:

    using PipelineLayoutSet = std::set<PipelineLayout<B>, std::less<>>;
    using PipelineLayoutConstIterator = typename PipelineLayoutSet::const_iterator;

    using PipelineMap = typename std::map<uint64_t, PipelineHandle<B>>;
    using PipelineMapConstIterator = typename PipelineMap::const_iterator;

    using DescriptorVariantType = std::variant<
        std::vector<DescriptorBufferInfo<B>>,
        std::vector<DescriptorImageInfo<B>>,
        std::vector<BufferViewHandle<B>>>;
    using DescriptorValueMapType = std::map<uint32_t, std::vector<std::tuple<DescriptorType<B>, DescriptorVariantType>>>;

    uint64_t internalCalculateHashKey() const;
    PipelineHandle<B> internalCreateGraphicsPipeline(uint64_t hashKey);
    PipelineMapConstIterator internalUpdateMap();

    AutoSaveJSONFileObject<PipelineConfiguration<B>> myConfig;
    PipelineLayoutSet myLayouts;
    PipelineCacheHandle<B> myCache = {}; // todo:: move pipeline cache to its own class, and pass in reference to it.
    PipelineMap myPipelineMap;
    DescriptorPoolHandle<B> myDescriptorPool = {};
    std::vector<DescriptorSetArray<B>> myDescriptorSets;

    // temp
    std::shared_ptr<PipelineResourceView<B>> myResources;
    //

    // graphics shadow state
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
    PipelineLayoutConstIterator myLayout = {};
    //

    // descriptor shadow state
    DescriptorValueMapType myDescriptorValueMap;
    //
    
    std::unique_ptr<XXH3_state_t, XXH_errorcode(*)(XXH3_state_t*)> myXXHState = { XXH3_createState(), XXH3_freeState };
};

#include "pipeline.inl"
#include "pipeline-vulkan.inl"
