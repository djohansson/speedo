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
#include <string>

template <GraphicsBackend B>
struct PipelineCacheHeader
{
};

template <GraphicsBackend B>
class PipelineLayout : public DeviceResource<B>
{
public:

    PipelineLayout() = default;
    PipelineLayout(PipelineLayout<B>&& other) noexcept;
    PipelineLayout(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const std::shared_ptr<ShaderReflectionInfo<B>>& shaderModule);
    ~PipelineLayout();

    PipelineLayout& operator=(PipelineLayout&& other) noexcept;
    operator auto() const { return myLayout; }
    bool operator==(const PipelineLayout& other) const { return myLayout == other; }
    bool operator<(const PipelineLayout& other) const { return myLayout < other; }

    void swap(PipelineLayout& rhs) noexcept;
	friend void swap(PipelineLayout& lhs, PipelineLayout& rhs) noexcept { lhs.swap(rhs); }

    const auto& getShaderModules() const { return myShaderModules; }
    const auto& getDescriptorSetLayouts() const { return myDescriptorSetLayouts; }

private:

    PipelineLayout( // takes ownership over provided handles
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        std::vector<ShaderModule<B>>&& shaderModules,
        DescriptorSetLayoutMapType<B>&& descriptorSetLayouts);
    PipelineLayout( // takes ownership over provided handles
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        std::vector<ShaderModule<B>>&& shaderModules,
        DescriptorSetLayoutMapType<B>&& descriptorSetLayouts,
        PipelineLayoutHandle<B>&& layout);

    std::vector<ShaderModule<B>> myShaderModules;
	DescriptorSetLayoutMapType<B> myDescriptorSetLayouts;
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

// todo: create single-thread / multi-thread interface:
//         * fork() pushes/copies/pops thread-specific state on stack
//         * descriptor pools created in groups for each thread instance
//         * pipeline map/cache shared across thread instances
//         * descriptor data shared across thread instances (if possible. avoids excessive copying...)
template <GraphicsBackend B>
class PipelineContext : public DeviceResource<B>
{
    using BindingVariantVectorType = std::variant<
        std::vector<DescriptorBufferInfo<B>>,
        std::vector<DescriptorImageInfo<B>>,
        std::vector<BufferViewHandle<B>>,
        std::vector<InlineUniformBlock<Vk>>>; // InlineUniformBlock can only have one array element per binding
    using BindingValueType = std::tuple<DescriptorType<B>, BindingVariantVectorType>;
    using BindingsMapType = UnorderedMapType<uint32_t, BindingValueType>; // [binding, data], perhaps make this an array?
    using DescriptorSetArrayListType = std::list<
        std::tuple<
            DescriptorSetArray<B>, // descriptor set array
            uint8_t, // current array index
            CopyableAtomic<uint32_t>>>; // reference count. // todo: c++20: std::atomic_ref
    using DescriptorMapType = UnorderedMapType<
        uint64_t, // set layout key. (todo: investigate if descriptor state should be part of this?)
        std::tuple<BindingsMapType, bool, std::optional<DescriptorSetArrayListType>>>; // [bindings, isDirty, descriptorSets (optional - if std::nullopt -> uses push descriptors)]
    using PipelineMapType = ConcurrentUnorderedMapType<
        uint64_t, // pipeline object key (pipeline layout + gfx/compute/raytrace state)
        CopyableAtomic<PipelineHandle<B>>, // todo: c++20: std::atomic_ref
        PassThroughHash<uint64_t>>;

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

    void bindPipeline(CommandBufferHandle<B> cmd, PipelineHandle<B> handle);

    void bindDescriptorSet(
        CommandBufferHandle<B> cmd,
        DescriptorSetHandle<B> handle,
        uint32_t set,
        std::optional<uint32_t> bufferOffset = std::nullopt);

    // "auto" api begin

    void bindPipelineAuto(CommandBufferHandle<B> cmd);

    void bindDescriptorSetAuto(
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

    template <typename T>
    void setDescriptorData(
        T&& data,
        DescriptorType<B> type,
        uint32_t set,
        const std::string& shaderVariableName);

    // array
    template <typename T>
    void setDescriptorData(
        std::vector<T>&& data,
        DescriptorType<B> type,
        uint32_t set,
        uint32_t binding);

    template <typename T>
    void setDescriptorData(
        std::vector<T>&& data,
        DescriptorType<B> type,
        uint32_t set,
        const std::string& shaderVariableName);

    // array-element
    template <typename T>
    void setDescriptorData(
        T&& data,
        DescriptorType<B> type,
        uint32_t set,
        uint32_t binding,
        uint32_t index);

    template <typename T>
    void setDescriptorData(
        T&& data,
        DescriptorType<B> type,
        uint32_t set,
        const std::string& shaderVariableName,
        uint32_t index);

    // "auto" api end
    
    // temp
    void setModel(const std::shared_ptr<Model<B>>& model); // todo: rewrite to use generic draw call structures / buffers
    auto& resources() { return myGraphicsState.resources; }
    //

private:
    
    // todo: create maps with sensible hash keys for each structure that goes into vkCreateGraphicsPipelines()
    //       combine them to get a compisite hash for the actual pipeline object (Merkle tree)
    //       might need more fine grained control here...
    void internalResetSharedState();
    void internalResetGraphicsInputState();
    void internalResetGraphicsRasterizationState();
    void internalResetGraphicsOutputState();
    void internalResetGraphicsDynamicState();
    //

    void internalPushDescriptorSet(
        CommandBufferHandle<B> cmd,
        uint32_t set,
        const BindingsMapType& bindingsMap) const;
    void internalWriteDescriptorSet(
        DescriptorSetLayoutMapType<Vk>::const_iterator setLayoutIt,
        typename DescriptorMapType::iterator descriptorMapIt);

    uint64_t internalCalculateHashKey() const;
    PipelineHandle<B> internalCreateGraphicsPipeline(uint64_t hashKey);
    PipelineHandle<B> internalGetPipeline();

    AutoSaveJSONFileObject<PipelineConfiguration<B>> myConfig;
    
    DescriptorPoolHandle<B> myDescriptorPool = {}; // todo: should be handled differently to cater for multithread and explicit binds.
    
    DescriptorMapType myDescriptorMap;
    PipelineMapType myPipelineMap; // todo: move pipeline map & cache to its own class, and pass in reference to it.
    PipelineCacheHandle<B> myCache = {}; // todo: move pipeline map & cache to its own class, and pass in reference to it.
    
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
