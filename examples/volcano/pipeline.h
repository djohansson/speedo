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
    PipelineLayout( // takes ownership over provided handles
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        std::vector<ShaderModule<B>>&& shaderModules,
        DescriptorSetLayoutMap<B>&& descriptorSetLayouts);
    PipelineLayout( // takes ownership over provided handles
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        std::vector<ShaderModule<B>>&& shaderModules,
        DescriptorSetLayoutMap<B>&& descriptorSetLayouts,
        PipelineLayoutHandle<B>&& layout);
    ~PipelineLayout();

    PipelineLayout& operator=(PipelineLayout&& other);
    operator auto() const { return myLayout; }
    bool operator==(const PipelineLayout& other) { return myLayout == other; }
    bool operator<(const PipelineLayout& other) { return myLayout < other; }

    const auto& getDescriptorSetLayouts() const { return myDescriptorSetLayouts; }
    const auto& getShaders() const { return myShaders; }

private:

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
    auto getLayout() const { return static_cast<PipelineLayoutHandle<B>>(*myLayoutIt.value()); }
    const auto& getRenderTarget() const { return myRenderTarget; }

    void setRenderTarget(const std::shared_ptr<RenderTarget<B>>& renderTarget);

    void bind(CommandBufferHandle<B> cmd);
    void bindDescriptorSet(
        CommandBufferHandle<B> cmd,
        uint8_t set,
        std::optional<uint32_t> bufferOffset = std::nullopt) const;
    void pushDescriptorSet(CommandBufferHandle<B> cmd, uint8_t set) const;

    // note scope 1: not quite sure yet on how we should interact with these functions. Perhaps some sort of layout begin/end that returns a proxy object so set all state?
    PipelineLayoutHandle<B> emplaceAndSetLayout(PipelineLayout<B>&& layout);

    void setLayout(PipelineLayoutHandle<B> handle);

    // object
    template <typename T>
    void setDescriptorData(
        T&& data,
        DescriptorType<B> type,
        uint8_t set,
        uint32_t binding);

    // array
    template <typename T>
    void setDescriptorData(
        std::vector<T>&& data,
        DescriptorType<B> type,
        uint8_t set,
        uint32_t binding);

    // array-element
    template <typename T>
    void setDescriptorData(
        T&& data,
        DescriptorType<B> type,
        uint8_t set,
        uint32_t binding,
        uint32_t index);
    
    // todo: ideally these should not be externally visible, but handled internally and "automagically" in this class.
    //void copyDescriptorSet(uint8_t set, DescriptorSetArray<B>& dst) const;
    
    void writeDescriptorSet(uint8_t set) const;
    //

    // note scope 1 end
    

    // temp
    void setModel(const std::shared_ptr<Model<B>>& model); // todo: rewrite to use generic draw call structures / buffers
    auto& resources() { return myGraphicsState.resources; }
    //

private:

    template <typename T>
    struct PassThroughHash
    {
        size_t operator()(const T& key) const { return static_cast<size_t>(key); }
    };

    using PipelineMap = ConcurrentMapType<
        uint64_t,
        CopyableAtomic<PipelineHandle<B>>,
        PassThroughHash<uint64_t>>;

    using PipelineLayoutSet = SetType<
        PipelineLayout<B>,
        robin_hood::hash<PipelineLayoutHandle<B>>,
        std::equal_to<>>;

    using BindingVariantVector = std::variant<
        std::vector<DescriptorBufferInfo<B>>,
        std::vector<DescriptorImageInfo<B>>,
        std::vector<BufferViewHandle<B>>,
        std::vector<InlineUniformBlock<Vk>>>; // InlineUniformBlock can only have one array element per binding
    using BindingValueType = std::tuple<DescriptorType<B>, BindingVariantVector>;
    using BindingsMap = MapType<uint32_t, BindingValueType>;

    using DescriptorMap = MapType<uint64_t, BindingsMap>;

    // todo: create maps with sensible hash keys for each structure that goes into vkCreateGraphicsPipelines()
    //       combine them to get a compisite hash for the actual pipeline object (Merkle tree)
    //       might need more fine grained control here...
    void internalResetSharedState();
    void internalResetGraphicsInputState();
    void internalResetGraphicsRasterizationState();
    void internalResetGraphicsOutputState();
    void internalResetGraphicsDynamicState();
    //

    uint64_t internalCalculateHashKey() const;
    PipelineHandle<B> internalCreateGraphicsPipeline(uint64_t hashKey);

    PipelineHandle<B> internalGetPipeline();
    const DescriptorSetHandle<Vk>& internalGetDescriptorSet(uint8_t set) const;

    static uint64_t internalMakeDescriptorKey(const PipelineLayout<B>& layout, uint8_t set);

    AutoSaveJSONFileObject<PipelineConfiguration<B>> myConfig;
    DescriptorPoolHandle<B> myDescriptorPool = {};
    PipelineCacheHandle<B> myCache = {}; // todo:: move pipeline cache to its own class, and pass in reference to it.
    PipelineLayoutSet myLayouts; // todo: do not store these in here. we can treat them the same way as render targets.
    PipelineMap myPipelineMap;
    DescriptorMap myDescriptorMap;
    std::array<std::optional<std::tuple<DescriptorSetArray<B>, uint32_t>>, 4> myDescriptorSets; // temp!

    // shared state
    PipelineBindPoint<B> myBindPoint = {};
    std::vector<PipelineShaderStageCreateInfo<B>> myShaderStages;
    std::optional<typename PipelineLayoutSet::const_iterator> myLayoutIt;
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
