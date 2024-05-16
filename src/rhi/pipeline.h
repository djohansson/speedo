#pragma once

#include "descriptorset.h"
#include "device.h"
#include "image.h"
#include "model.h"
#include "rendertarget.h"
#include "shader.h"
#include "types.h"

#include <core/file.h>
#include <core/utils.h>

#include <memory>
#include <optional>
#include <string>

template <GraphicsApi G>
class Pipeline;

template <GraphicsApi G>
class PipelineLayout final : public DeviceObject<G>
{
public:
	constexpr PipelineLayout() noexcept = default;
	PipelineLayout(PipelineLayout<G>&& other) noexcept;
	~PipelineLayout();

	PipelineLayout& operator=(PipelineLayout&& other) noexcept;
	operator auto() const noexcept { return myLayout; }
	bool operator==(const PipelineLayout& other) const noexcept { return myLayout == other; }
	bool operator<(const PipelineLayout& other) const noexcept { return myLayout < other; }

	void Swap(PipelineLayout& rhs) noexcept;
	friend void Swap(PipelineLayout& lhs, PipelineLayout& rhs) noexcept { lhs.Swap(rhs); }

	const auto& GetShaderModules() const noexcept { return myShaderModules; }
	const auto& GetDescriptorSetLayouts() const noexcept { return myDescriptorSetLayouts; }
	const DescriptorSetLayout<G>& GetDescriptorSetLayout(uint32_t set) const noexcept;

private:
	friend Pipeline<G>;
	PipelineLayout(
		const std::shared_ptr<Device<G>>& device, const ShaderSet<G>& shaderSet);
	PipelineLayout( // takes ownership over provided handles
		const std::shared_ptr<Device<G>>& device,
		std::vector<ShaderModule<G>>&& shaderModules,
		DescriptorSetLayoutFlatMap<G>&& descriptorSetLayouts);
	PipelineLayout( // takes ownership over provided handles
		const std::shared_ptr<Device<G>>& device,
		std::vector<ShaderModule<G>>&& shaderModules,
		DescriptorSetLayoutFlatMap<G>&& descriptorSetLayouts,
		PipelineLayoutHandle<G>&& layout);

	std::vector<ShaderModule<G>> myShaderModules;
	DescriptorSetLayoutFlatMap<G> myDescriptorSetLayouts;
	PipelineLayoutHandle<G> myLayout{};
};

template <GraphicsApi G>
struct PipelineResourceView
{
	// begin temp
	std::shared_ptr<Model<G>> model;
	std::shared_ptr<Image<G>> black;
	std::shared_ptr<ImageView<G>> blackImageView;
	std::shared_ptr<Image<G>> image;
	std::shared_ptr<ImageView<G>> imageView;
	std::shared_ptr<SamplerVector<G>> samplers;
	// end temp
};

template <GraphicsApi G>
struct PipelineCacheHeader
{};

template <GraphicsApi G>
struct PipelineConfiguration
{
	std::string cachePath;
};

// todo: create single-thread / multi-thread interface:
//         * fork() pushes/copies/pops thread-specific state on stack
//         * descriptor pools created in groups for each thread instance
//         * pipeline map/cache shared across thread instances
//         * descriptor data shared across thread instances (if possible. avoids excessive copying...)
template <GraphicsApi G>
class Pipeline : public DeviceObject<G>
{
	using PipelineMapType = UnorderedMap<
		uint64_t, // pipeline object key (pipeline layout + gfx/compute/raytrace state)
		CopyableAtomic<PipelineHandle<G>>,
		IdentityHash<uint64_t>>;

	using DescriptorMapType = UnorderedMap<
		DescriptorSetLayoutHandle<G>, // todo: hash DescriptorSetData into this key? monitor mem usage, and find good strategy for recycling memory and to what level we should cache this data after being consumed.
		DescriptorSetState<G>>;

public:
	Pipeline(
		const std::shared_ptr<Device<G>>& device,
		PipelineConfiguration<G>&& defaultConfig = {});
	~Pipeline();

	const auto& GetConfig() const noexcept { return myConfig; }
	auto GetCache() const noexcept { return myCache; }
	auto GetDescriptorPool() const noexcept { return myDescriptorPool; }
	auto GetBindPoint() const noexcept { return myBindPoint; }
	auto GetLayout() const noexcept { return myLayout; }

	PipelineLayoutHandle<G> CreateLayout(const ShaderSet<G>& shaderSet);

	// "manual" api

	void BindPipeline(
		CommandBufferHandle<G> cmd, PipelineBindPoint<G> bindPoint, PipelineHandle<G> handle) const;

	void BindDescriptorSet(
		CommandBufferHandle<G> cmd,
		DescriptorSetHandle<G> handle,
		PipelineBindPoint<G> bindPoint,
		PipelineLayoutHandle<G> layoutHandle,
		uint32_t set,
		std::optional<uint32_t> bufferOffset = std::nullopt) const;

	// "auto" api

	void BindPipelineAuto(CommandBufferHandle<G> cmd);

	void BindLayoutAuto(PipelineLayoutHandle<G> layout, PipelineBindPoint<G> bindPoint);

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

	// temp
	auto& GetRenderTarget() const noexcept
	{
		ASSERT(myRenderTarget);
		return *myRenderTarget;
	}

	void SetRenderTarget(const std::shared_ptr<RenderTarget<G>>& renderTarget);
	void SetModel(const std::shared_ptr<Model<G>>& model); // todo: rewrite to use generic draw call structures / buffers

	auto& GetResources() noexcept { return myGraphicsState.resources; }
	const auto& GetResources() const noexcept { return myGraphicsState.resources; }
	//

	// "auto" api end	

private:
	// todo: create maps with sensible hash keys for each structure that goes into vkCreateGraphicsPipelines()
	//       combine them to get a compisite hash for the actual pipeline object (Merkle tree)
	//       might need more fine grained control here...
	void InternalResetState();
	void InternalPrepareDescriptorSets();
	void InternalResetGraphicsState();
	void InternalResetComputeState();
	//

	void InternalUpdateDescriptorSet(
		const DescriptorSetLayout<G>& BindLayoutAuto,
		const BindingsData<G>& bindingsData,
		const DescriptorUpdateTemplate<G>& setTemplate,
		DescriptorSetArrayList<G>& setArrayList);
	void InternalPushDescriptorSet(
		CommandBufferHandle<G> cmd,
		const BindingsData<G>& bindingsData,
		const DescriptorUpdateTemplate<G>& setTemplate) const;

	static void InternalUpdateDescriptorSetTemplate(
		const BindingsMap<G>& bindingsMap, DescriptorUpdateTemplate<G>& setTemplate);

	uint64_t InternalCalculateHashKey() const;
	PipelineHandle<G> InternalCreateGraphicsPipeline(uint64_t hashKey);
	PipelineHandle<G> InternalGetPipeline();
	const PipelineLayout<G>& InternalGetLayout();

	file::Object<PipelineConfiguration<G>, file::AccessMode::kReadWrite, true> myConfig;

	DescriptorMapType myDescriptorMap;
	
	// todo: should be handled differently to cater for multithread and explicit binds.
	DescriptorPoolHandle<G> myDescriptorPool{};

	// todo: move pipeline map & cache to its own class, and pass in reference to it.
	PipelineMapType myPipelineMap; 
	PipelineCacheHandle<G> myCache{};

	// shared state
	PipelineBindPoint<G> myBindPoint{};
	PipelineLayoutHandle<G> myLayout{};
	std::shared_ptr<RenderTarget<G>> myRenderTarget;
	// end shared state

	struct GraphicsState
	{
		std::vector<PipelineShaderStageCreateInfo<G>> shaderStages;
		uint32_t shaderStageFlags{};
		PipelineVertexInputStateCreateInfo<G> vertexInput{};
		PipelineInputAssemblyStateCreateInfo<G> inputAssembly{};
		std::vector<Viewport<G>> viewports;
		std::vector<Rect2D<G>> scissorRects;
		PipelineViewportStateCreateInfo<G> viewport{};
		PipelineRasterizationStateCreateInfo<G> rasterization{};
		PipelineMultisampleStateCreateInfo<G> multisample{};
		PipelineDepthStencilStateCreateInfo<G> depthStencil{};
		std::vector<PipelineColorBlendAttachmentState<G>> colorBlendAttachments{};
		PipelineColorBlendStateCreateInfo<G> colorBlend{};
		std::vector<DynamicState<G>> dynamicStateDescs;
		PipelineDynamicStateCreateInfo<G> dynamicState{};
		// temp?
		PipelineResourceView<G> resources;
		UnorderedMap<PipelineLayoutHandle<G>, PipelineLayout<G>> layouts;
		//UnorderedSet<RenderTarget<G>> renderTargets;
		//
	} myGraphicsState{};

	struct ComputeState
	{
		// todo:
	} myComputeState{};

	struct RayTracingState
	{
		// todo:
	} myRayTracingState{};
};

#include "pipeline.inl"
