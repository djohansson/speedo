#pragma once

#include "descriptorset.h"
#include "device.h"
#include "file.h"
#include "image.h"
#include "model.h"
#include "rendertarget.h"
#include "shader.h"
#include "types.h"
#include "utils.h"

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

    PipelineLayout(PipelineLayout<B>&& other);
    PipelineLayout(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const std::shared_ptr<ShaderReflectionInfo<B>>& shaderModule);
    ~PipelineLayout();

    PipelineLayout& operator=(PipelineLayout&& other);
    operator auto() const { return myLayout; }
    bool operator==(const PipelineLayout& other) const { return myLayout == other; }
    bool operator<(const PipelineLayout& other) const { return myLayout < other; }

    const auto& getDescriptorSetLayouts() const { return myDescriptorSetLayouts; }
    const auto& getShaders() const { return myShaders; }

private:

    PipelineLayout( // takes ownership over provided handles
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        std::vector<ShaderModule<B>>&& shaderModules,
        DescriptorSetLayoutMap<B>&& descriptorSetLayouts);
    PipelineLayout( // takes ownership over provided handles
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        std::vector<ShaderModule<B>>&& shaderModules,
        DescriptorSetLayoutMap<B>&& descriptorSetLayouts,
        PipelineLayoutHandle<B>&& layout);

    std::vector<ShaderModule<B>> myShaders;
	DescriptorSetLayoutMap<B> myDescriptorSetLayouts;
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
};

template <GraphicsBackend B>
struct PipelineConfiguration : DeviceResourceCreateDesc<B>
{
    std::filesystem::path cachePath;
};

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
    
    const auto& getConfig() const { return myConfig; }
    auto getCache() const { return myCache; }
    auto getDescriptorPool() const { return myDescriptorPool; }
    const auto& getLayout() const { return myLayout; }
    const auto& getRenderTarget() const { return myRenderTarget; }

    void setLayout(const std::shared_ptr<PipelineLayout<B>>& layout);
    void setRenderTarget(const std::shared_ptr<RenderTarget<B>>& renderTarget);

    void bindPipeline(CommandBufferHandle<B> cmd);
    void bindDescriptorSet(
        CommandBufferHandle<B> cmd,
        uint32_t set,
        std::optional<uint32_t> bufferOffset = std::nullopt);

    // object
    template <typename T>
    void setDescriptorData(
        T&& data,
        DescriptorType<B> type,
        uint32_t set,
        uint32_t binding);

    // array
    template <typename T>
    void setDescriptorData(
        std::vector<T>&& data,
        DescriptorType<B> type,
        uint32_t set,
        uint32_t binding);

    // array-element
    template <typename T>
    void setDescriptorData(
        T&& data,
        DescriptorType<B> type,
        uint32_t set,
        uint32_t binding,
        uint32_t index);
    
    // temp
    void setModel(const std::shared_ptr<Model<B>>& model); // todo: rewrite to use generic draw call structures / buffers
    auto& resources() { return myGraphicsState.resources; }
    //

private:
    using BindingVariantVector = std::variant<
        std::vector<DescriptorBufferInfo<B>>,
        std::vector<DescriptorImageInfo<B>>,
        std::vector<BufferViewHandle<B>>,
        std::vector<InlineUniformBlock<Vk>>>; // InlineUniformBlock can only have one array element per binding
    using BindingValueType = std::tuple<DescriptorType<B>, BindingVariantVector>;
    using BindingsMap = UnorderedMapType<uint32_t, BindingValueType>; // [binding, data], perhaps make this an array?
    using DescriptorSetArrayList = std::list<
        std::tuple<
            DescriptorSetArray<B>, // descriptor set array
            uint8_t>>; // current array index
    using DescriptorMap = UnorderedMapType<
        uint64_t, // setLayout key
        std::tuple<
            std::tuple<BindingsMap, bool>, // [bindings, isDirty]
            std::optional<DescriptorSetArrayList>>>; // optional descriptor sets - if std::nullopt -> uses push descriptors
    using PipelineMap = ConcurrentUnorderedMapType<
        uint64_t, // pipeline object key (pipeline layout + gfx/compute/raytrace state)
        CopyableAtomic<PipelineHandle<B>>,
        PassThroughHash<uint64_t>>;
    
    // todo: create maps with sensible hash keys for each structure that goes into vkCreateGraphicsPipelines()
    //       combine them to get a compisite hash for the actual pipeline object (Merkle tree)
    //       might need more fine grained control here...
    void internalResetSharedState();
    void internalResetGraphicsInputState();
    void internalResetGraphicsRasterizationState();
    void internalResetGraphicsOutputState();
    void internalResetGraphicsDynamicState();
    //

    void internalPushDescriptorSet(CommandBufferHandle<B> cmd, uint32_t set) const;
    void internalWriteDescriptorSet(uint32_t set);

    uint64_t internalCalculateHashKey() const;
    PipelineHandle<B> internalCreateGraphicsPipeline(uint64_t hashKey);
    PipelineHandle<B> internalGetPipeline();

    AutoSaveJSONFileObject<PipelineConfiguration<B>> myConfig;
    DescriptorPoolHandle<B> myDescriptorPool = {};
    PipelineCacheHandle<B> myCache = {}; // todo:: move pipeline cache to its own class, and pass in reference to it.
    DescriptorMap myDescriptorMap;
    PipelineMap myPipelineMap;
    
    // shared state
    PipelineBindPoint<B> myBindPoint = {};
    std::vector<PipelineShaderStageCreateInfo<B>> myShaderStages;
    std::shared_ptr<PipelineLayout<B>> myLayout;
    std::shared_ptr<RenderTarget<B>> myRenderTarget;
    // end shared state

    struct GraphicsState
    {
        PipelineVertexInputStateCreateInfo<B> vertexInput = {};
        PipelineInputAssemblyStateCreateInfo<B> inputAssembly = {};
        std::vector<Viewport<B>> viewports;
        std::vector<Rect2D<B>> scissorRects;
        PipelineViewportStateCreateInfo<B> viewport = {};
        PipelineRasterizationStateCreateInfo<B> rasterization = {};
        PipelineMultisampleStateCreateInfo<B> multisample = {};
        PipelineDepthStencilStateCreateInfo<B> depthStencil = {};
        std::vector<PipelineColorBlendAttachmentState<B>> colorBlendAttachments = {};
        PipelineColorBlendStateCreateInfo<B> colorBlend = {};
        std::vector<DynamicState<B>> dynamicStateDescs;
        PipelineDynamicStateCreateInfo<B> dynamicState = {};
        // temp
        PipelineResourceView<B> resources;
        //
    } myGraphicsState = {};

    struct ComputeState
    {
        // todo:
    } myComputeState = {};

    struct RayTracingState
    {
        // todo:
    } myRayTracingState = {};
};

#include "pipeline.inl"
#include "pipeline-vulkan.inl"
