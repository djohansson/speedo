#pragma once

#include <core/utils.h>

#include <rhi/descriptorset.h>
#include <rhi/deviceobject.h>
#include <rhi/rendertarget.h>
#include <rhi/shader.h>
#include <rhi/types.h>

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>

namespace rhi
{

template <GraphicsApi G>
class Pipeline;

template <GraphicsApi G>
class PipelineLayout;

template <GraphicsApi G>
struct PipelineLayoutCreateDesc final : DeviceObjectCreateDesc<G>
{
	std::string cachePath;
};

template <GraphicsApi G>
struct ObjectTraits<PipelineLayout<G>>
{
	using CreateDescType = PipelineLayoutCreateDesc<G>;
};

template <GraphicsApi G>
class PipelineLayout final : public DeviceObject<PipelineLayout<G>>
{
public:
	using SuperType = DeviceObject<PipelineLayout<G>>;
	using CreateDescType = ObjectTraits<PipelineLayout<G>>::CreateDescType;

	constexpr PipelineLayout() noexcept = default;
	PipelineLayout(PipelineLayout&& other) noexcept;
	~PipelineLayout();

	[[maybe_unused]] PipelineLayout& operator=(PipelineLayout&& other) noexcept;
	[[nodiscard]] operator auto() const noexcept { return myLayout; }//NOLINT(google-explicit-constructor)
	[[nodiscard]] bool operator==(const PipelineLayout& other) const noexcept { return myLayout == other.myLayout; }
	[[nodiscard]] bool operator<(const PipelineLayout& other) const noexcept { return myLayout < other.myLayout; }

	void Swap(PipelineLayout& rhs) noexcept;
	friend void Swap(PipelineLayout& lhs, PipelineLayout& rhs) noexcept { lhs.Swap(rhs); }

	[[nodiscard]] const auto& GetShaderModules() const noexcept { return myShaderModules; }
	[[nodiscard]] const auto& GetDescriptorSetLayouts() const noexcept { return myDescriptorSetLayouts; }
	[[nodiscard]] const DescriptorSetLayout<G>& GetDescriptorSetLayout(uint32_t set) const noexcept;
	[[nodiscard]] const auto& GetPushConstantRanges() const noexcept { return myPushConstantRanges; }

private:
	friend Pipeline<G>;
	explicit PipelineLayout(
		CreateDescType&& desc,
		const ShaderSet& shaderSet);
	PipelineLayout( // takes ownership over provided handles
		CreateDescType&& desc,
		std::vector<ShaderModule<G>>&& shaderModules,
		DescriptorSetLayoutFlatMap<G>&& descriptorSetLayouts);
	PipelineLayout( // takes ownership over provided handles
		CreateDescType&& desc,
		std::vector<ShaderModule<G>>&& shaderModules,
		DescriptorSetLayoutFlatMap<G>&& descriptorSetLayouts,
		PipelineLayoutHandle<G>&& layout);

	std::vector<ShaderModule<G>> myShaderModules;
	DescriptorSetLayoutFlatMap<G> myDescriptorSetLayouts;
	std::vector<PushConstantRange<G>> myPushConstantRanges; // from myDescriptorSetLayouts, as the layout was created with
	PipelineLayoutHandle<G> myLayout{};
};

template <GraphicsApi G>
struct PipelineCacheHeader
{};

template <GraphicsApi G>
struct PipelineCreateDesc final : DeviceObjectCreateDesc<G>
{
	std::string cachePath;
	std::vector<DescriptorPoolSize> descriptorPoolSizes; // of the descriptor pool its descriptor sets come from
};

template <GraphicsApi G>
struct ObjectTraits<Pipeline<G>>
{
	using CreateDescType = PipelineCreateDesc<G>;
};

// todo: create single-thread / multi-thread interface:
//         * fork() pushes/copies/pops thread-specific state on stack
//         * descriptor pools created in groups for each thread instance
//         * pipeline map/cache shared across thread instances
//         * descriptor data shared across thread instances (if possible. avoids excessive copying...)
template <GraphicsApi G>
class Pipeline final : public DeviceObject<Pipeline<G>>
{
	using PipelineMapType = core::UnorderedMap<
		uint64_t, // pipeline object key (pipeline layout + gfx/compute/raytrace state)
		PipelineHandle<G>,
		core::IdentityHash<uint64_t>>;

	using PipelineLayoutSetType = core::UnorderedSet<
		PipelineLayout<G>,
		core::HandleHash<PipelineLayout<G>, PipelineLayoutHandle<G>>,
		core::HandleCompareEqualTo<PipelineLayout<G>, PipelineLayoutHandle<G>>>;

	using DescriptorMapType = core::UnorderedMap<
		DescriptorSetLayoutHandle<G>, // todo: monitor mem usage, and find good strategy for recycling memory and to what level we should cache this data after being consumed.
		DescriptorSetState<G>>;

public:
	using SuperType = DeviceObject<Pipeline<G>>;
	using CreateDescType = ObjectTraits<Pipeline<G>>::CreateDescType;

	constexpr Pipeline() noexcept = default;
	explicit Pipeline(CreateDescType&& desc);
	Pipeline(Pipeline&& other) noexcept;
	~Pipeline();

	[[maybe_unused]] Pipeline& operator=(Pipeline&& other) noexcept;

	void Swap(Pipeline& rhs) noexcept;
	friend void Swap(Pipeline& lhs, Pipeline& rhs) noexcept { lhs.Swap(rhs); }

	[[nodiscard]] auto GetCache() const noexcept { return myCache; }
	[[nodiscard]] auto GetDescriptorPool() const noexcept { return myDescriptorPool; }
	[[nodiscard]] auto GetBindPoint() const noexcept { return myBindPoint; }
	[[nodiscard]] PipelineLayoutHandle<G> GetLayout() const noexcept;

	// pushes data at offset into the current layout, with the stage flags of the push constant ranges it overlaps
	void PushConstants(CommandBufferHandle<G> cmd, std::span<const std::byte> data, uint32_t offset = 0) const;

	[[maybe_unused]] PipelineLayoutHandle<G> CreateLayout(const ShaderSet& shaderSet);

	// "manual" api

	void BindPipeline(
		CommandBufferHandle<G> cmd,
		PipelineBindPoint bindPoint,
		PipelineHandle<G> handle) const;

	void BindDescriptorSet(
		CommandBufferHandle<G> cmd,
		DescriptorSetHandle<G> handle,
		PipelineBindPoint bindPoint,
		PipelineLayoutHandle<G> layoutHandle,
		uint32_t set,
		std::optional<uint32_t> bufferOffset = std::nullopt) const;

	// "auto" api

	// a graphics pipeline is created per variant (topology and blending, cached): it is a parameter rather than state,
	// since the draw threads share the pipeline object
	[[maybe_unused]] PipelineHandle<G> BindPipelineAuto(CommandBufferHandle<G> cmd, GraphicsPipelineVariant variant = {}); // todo: make implicit and call internally whenever relevant state changes
	// a compute pipeline is created per variant (the layout's compute entry point, cached), as a graphics one is
	[[maybe_unused]] PipelineHandle<G> BindPipelineAuto(CommandBufferHandle<G> cmd, ComputePipelineVariant variant);

	// a graphics pipeline for the bound layout and render target, as BindPipelineAuto would create it, without waiting
	// for it: its handle if it exists; else none, and the first time it is asked for, a job that creates it from a copy of
	// the current state (to run on any thread: until it has, BindPipelineAuto for the variant waits for it)
	struct GraphicsPipelineRequest
	{
		PipelineHandle<G> handle{};
		std::function<void()> create;
	};
	[[nodiscard]] GraphicsPipelineRequest RequestGraphicsPipeline(GraphicsPipelineVariant variant);
	// the thread group size (from the shader's reflection) of one of the bound compute layout's entry points, to dispatch
	// it by threads (see CommandEncoder::DispatchThreads)
	[[nodiscard]] const ComputeLaunchParameters& GetComputeLaunchParameters(ComputePipelineVariant variant = {}) const;

	void BindLayoutAuto(PipelineLayoutHandle<G> layout, PipelineBindPoint bindPoint);

	void BindDescriptorSetAuto(
		CommandBufferHandle<G> cmd,
		uint32_t set,
		std::optional<uint32_t> bufferOffset = std::nullopt);

	template <typename T>
	void SetDescriptorData(
		uint64_t shaderVariableNameHash, const DescriptorSetLayout<G>& layout, T&& data);

	template <typename T>
	void SetDescriptorData(std::string_view shaderVariableName, T&& data, uint32_t set);

	template <typename T>
	void SetDescriptorData(
		uint64_t shaderVariableNameHash,
		const DescriptorSetLayout<G>& layout,
		const std::vector<T>& data);

	template <typename T>
	void SetDescriptorData(
		std::string_view shaderVariableName, const std::vector<T>& data, uint32_t set);

	template <typename T>
	void SetDescriptorData(
		uint64_t shaderVariableNameHash,
		const DescriptorSetLayout<G>& layout,
		T&& data,
		uint32_t index);

	template <typename T>
	void SetDescriptorData(
		std::string_view shaderVariableName,
		T&& data,
		uint32_t set,
		uint32_t index);	

	void SetRenderTarget(IRenderTarget<G>& renderTarget);
	//

	// "auto" api end	

private:
	// todo: create maps with sensible hash keys for each structure that goes into vkCreateGraphicsPipelines()
	//       combine them to get a compisite hash for the actual pipeline object (Merkle tree)
	//       might need more fine grained control here...
	void InternalResetDescriptorPool();
	void InternalResetGraphicsState();
	void InternalResetComputeState();
	//

	void InternalPrepareDescriptorSets();

	void InternalUpdateDescriptorSet(
		const DescriptorSetLayout<G>& BindLayoutAuto,
		const BindingsData<G>& bindingsData,
		const DescriptorUpdateTemplate<G>& setTemplate,
		DescriptorSetArrayList<G>& setArrayList) const;
	static void InternalPushDescriptorSet(
		CommandBufferHandle<G> cmd,
		PipelineLayoutHandle<kVk> layout,
		const BindingsData<G>& bindingsData,
		const DescriptorUpdateTemplate<G>& setTemplate);
	static void InternalUpdateDescriptorSetTemplate(
		const BindingsMap<G>& bindingsMap,
		DescriptorUpdateTemplate<G>& setTemplate);

	[[nodiscard]] uint64_t InternalCalculateHashKey(GraphicsPipelineVariant variant, ComputePipelineVariant computeVariant = {}) const;
	// the graphics state a pipeline is created from, copied (see the backend)
	struct GraphicsSnapshot;
	[[nodiscard]] std::shared_ptr<const GraphicsSnapshot> InternalSnapshotGraphicsState() const;
	[[nodiscard]] PipelineHandle<G> InternalCreateGraphicsPipeline(
		uint64_t hashKey, GraphicsPipelineVariant variant, const GraphicsSnapshot& snapshot) const;
	[[nodiscard]] PipelineHandle<G> InternalCreateComputePipeline(uint64_t hashKey, ComputePipelineVariant variant);
	[[nodiscard]] PipelineHandle<G> InternalGetPipeline(GraphicsPipelineVariant variant, ComputePipelineVariant computeVariant = {});
	[[nodiscard]] auto InternalGetLayout() const noexcept { return myCurrentLayoutIt; }

	DescriptorMapType myDescriptorMap;
	
	// todo: should be handled differently to cater for multithread and explicit binds.
	DescriptorPoolHandle<G> myDescriptorPool{};

	// todo: move pipeline map & cache to its own class, and pass in reference to it.
	PipelineMapType myPipelineMap; // a null handle while it is being created
	// guards myPipelineMap: draw threads and creation jobs (see RequestGraphicsPipeline) use it. created signals a handle
	// stored
	struct PipelineMapSync
	{
		std::mutex mutex;
		std::condition_variable created;
	};
	std::unique_ptr<PipelineMapSync> myPipelineMapSync = std::make_unique<PipelineMapSync>();
	PipelineCacheHandle<G> myCache{};

	// auto api shared state
	PipelineBindPoint myBindPoint{};
	RenderTargetPassHandle<G> myRenderTarget;
	PipelineLayoutSetType myPipelineLayouts;
	typename PipelineLayoutSetType::iterator myCurrentLayoutIt{};
	// end auto api shared state

	struct GraphicsState
	{
		std::vector<PipelineShaderStageCreateInfo<G>> shaderStages;
		uint32_t shaderStageFlags{};
		PipelineVertexInputStateCreateInfo<G> vertexInput{};
		PipelineInputAssemblyStateCreateInfo<G> inputAssembly{};
		std::vector<NativeViewport<G>> viewports;
		std::vector<NativeRect<G>> scissorRects;
		PipelineViewportStateCreateInfo<G> viewport{};
		PipelineRasterizationStateCreateInfo<G> rasterization{};
		PipelineMultisampleStateCreateInfo<G> multisample{};
		PipelineDepthStencilStateCreateInfo<G> depthStencil{};
		std::vector<PipelineColorBlendAttachmentState<G>> colorBlendAttachments{};
		PipelineColorBlendStateCreateInfo<G> colorBlend{};
		std::vector<DynamicState<G>> dynamicStateDescs;
		PipelineDynamicStateCreateInfo<G> dynamicState{};
		std::optional<PipelineRenderingCreateInfo<G>> dynamicRendering{};
	} myGraphicsState{};

	struct ComputeState
	{
		// per compute entry point of the bound layout, in its order (see ComputePipelineVariant)
		std::vector<PipelineShaderStageCreateInfo<G>> shaderStages;
		std::vector<ComputeLaunchParameters> launchParameters;
	} myComputeState{};

	struct RayTracingState
	{
		// todo:
	} myRayTracingState{};
};

} // namespace rhi

#include "pipeline.inl"
