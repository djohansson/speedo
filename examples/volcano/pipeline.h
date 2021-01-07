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
    // probably do not have these as shared ptrs, since we likely want pipeline to own them.
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
    operator auto() { return internalGetPipeline(); };

    const auto& getConfig() const { return myConfig; }
    auto getCache() const { return myCache; }
    auto getDescriptorPool() const { return myDescriptorPool; }
    auto getCurrentLayout() const { return static_cast<PipelineLayoutHandle<B>>(*myCurrentLayout.value()); }

    // note scope 1: not quite sure yet on how we should interact with these functions. Perhaps some sort of layout begin/end that returns a proxy object so set all state?
    void bind(CommandBufferHandle<B> cmd);
    void bindDescriptorSet(
        CommandBufferHandle<B> cmd,
        uint8_t set,
        std::optional<uint32_t> bufferOffset = std::nullopt) const;

    PipelineLayoutHandle<B> emplaceLayout(PipelineLayout<B>&& layout);

    void setCurrentLayout(PipelineLayoutHandle<B> handle);
    
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
    
    // todo: these should not be externally visible, but handled internally and "automagically" in this class.
    //void copyDescriptorSet(uint8_t set, DescriptorSetArray<B>& dst) const;
    void pushDescriptorSet(CommandBufferHandle<B> cmd, uint8_t set) const;
    void writeDescriptorSet(uint8_t set) const;
    //

    // note scope 1 end
    

    // temp
    const auto& resources() const { return myResources; }
    auto& renderTarget() { return myRenderTarget; }
    //

private:

    using PipelineMap = ConcurrentMapType<uint64_t, CopyableAtomic<PipelineHandle<B>>>;

    using PipelineLayoutSet = SetType<PipelineLayout<B>, robin_hood::hash<PipelineLayoutHandle<B>>, std::equal_to<>>;

    using BindingVariantVector = std::variant<
        std::vector<DescriptorBufferInfo<B>>,
        std::vector<DescriptorImageInfo<B>>,
        std::vector<BufferViewHandle<B>>,
        std::vector<InlineUniformBlock<Vk>>>; // InlineUniformBlock can only have one array element per binding
    using BindingValueType = std::tuple<DescriptorType<B>, BindingVariantVector>;
    using BindingsMap = MapType<uint32_t, BindingValueType>;

    using DescriptorMap = MapType<uint64_t, BindingsMap>;

    // temp - need more fine grained control here
    void internalResetLayoutState();
    void internalResetResourceState();
    void internalResetRasterizationState();
    void internalResetAllState();
    //

    uint64_t internalCalculateHashKey() const;
    PipelineHandle<B> internalCreateGraphicsPipeline(uint64_t hashKey);

    PipelineHandle<B> internalGetPipeline();
    const DescriptorSetHandle<Vk>& internalGetDescriptorSet(uint8_t set) const;

    static uint64_t internalMakeDescriptorKey(const PipelineLayout<B>& layout, uint8_t set);

    AutoSaveJSONFileObject<PipelineConfiguration<B>> myConfig;
    DescriptorPoolHandle<B> myDescriptorPool = {};
    PipelineCacheHandle<B> myCache = {}; // todo:: move pipeline cache to its own class, and pass in reference to it.
    PipelineLayoutSet myLayouts;
    PipelineMap myPipelineMap;
    DescriptorMap myDescriptorMap;
    std::array<std::optional<std::tuple<DescriptorSetArray<B>, uint32_t>>, 4> myDescriptorSets; // temp!

    // shared state
    PipelineBindPoint<B> myBindPoint = {};
    std::optional<typename PipelineLayoutSet::const_iterator> myCurrentLayout;
    std::shared_ptr<RenderTarget<B>> myRenderTarget;
    // end shared state

    // graphics state
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
    // temp
    std::shared_ptr<PipelineResourceView<B>> myResources;
    //
    // end graphics state

    // todo: compute shadow state

    // todo: raytrace shadow state
};

#include "pipeline.inl"
#include "pipeline-vulkan.inl"
