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

	void swap(PipelineLayout& rhs) noexcept;
	friend void swap(PipelineLayout& lhs, PipelineLayout& rhs) noexcept { lhs.swap(rhs); }

	const auto& getShaderModules() const noexcept { return myShaderModules; }
	const auto& getDescriptorSetLayouts() const noexcept { return myDescriptorSetLayouts; }
	const DescriptorSetLayout<G>& getDescriptorSetLayout(uint32_t set) const noexcept;

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

	GLZ_LOCAL_META(PipelineConfiguration<G>, cachePath);
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

	const auto& getConfig() const noexcept { return myConfig; }
	auto getCache() const noexcept { return myCache; }
	auto getDescriptorPool() const noexcept { return myDescriptorPool; }
	auto getBindPoint() const noexcept { return myBindPoint; }
	auto getLayout() const noexcept { return myLayout; }

	PipelineLayoutHandle<G> createLayout(const ShaderSet<G>& shaderSet);

	// "manual" api

	void bindPipeline(
		CommandBufferHandle<G> cmd, PipelineBindPoint<G> bindPoint, PipelineHandle<G> handle) const;

	void bindDescriptorSet(
		CommandBufferHandle<G> cmd,
		DescriptorSetHandle<G> handle,
		PipelineBindPoint<G> bindPoint,
		PipelineLayoutHandle<G> layoutHandle,
		uint32_t set,
		std::optional<uint32_t> bufferOffset = std::nullopt) const;

	// "auto" api

	void bindPipelineAuto(CommandBufferHandle<G> cmd);

	void bindLayoutAuto(PipelineLayoutHandle<G> layout, PipelineBindPoint<G> bindPoint);

	void bindDescriptorSetAuto(
		CommandBufferHandle<G> cmd,
		uint32_t set,
		std::optional<uint32_t> bufferOffset = std::nullopt);

	template <typename T>
	void setDescriptorData(
		uint64_t shaderVariableNameHash, const DescriptorSetLayout<G>& layout, T&& data);

	template <typename T>
	void setDescriptorData(std::string_view shaderVariableName, T&& data, uint32_t set);

	template <typename T>
	void setDescriptorData(
		uint64_t shaderVariableNameHash,
		const DescriptorSetLayout<G>& layout,
		const std::vector<T>& data);

	template <typename T>
	void setDescriptorData(
		std::string_view shaderVariableName, const std::vector<T>& data, uint32_t set);

	template <typename T>
	void setDescriptorData(
		uint64_t shaderVariableNameHash,
		const DescriptorSetLayout<G>& layout,
		T&& data,
		uint32_t index);

	template <typename T>
	void setDescriptorData(
		std::string_view shaderVariableName,
		T&& data,
		uint32_t set,
		uint32_t index);	

	// temp
	auto& getRenderTarget() const noexcept
	{
		assert(myRenderTarget);
		return *myRenderTarget;
	}

	void setRenderTarget(const std::shared_ptr<RenderTarget<G>>& renderTarget);
	void setModel(const std::shared_ptr<Model<G>>& model); // todo: rewrite to use generic draw call structures / buffers

	auto& resources() noexcept { return myGraphicsState.resources; }
	//

	// "auto" api end	

private:
	// todo: create maps with sensible hash keys for each structure that goes into vkCreateGraphicsPipelines()
	//       combine them to get a compisite hash for the actual pipeline object (Merkle tree)
	//       might need more fine grained control here...
	void internalResetState();
	void internalPrepareDescriptorSets();
	void internalResetGraphicsState();
	void internalResetComputeState();
	//

	void internalUpdateDescriptorSet(
		const DescriptorSetLayout<G>& bindLayoutAuto,
		const BindingsData<G>& bindingsData,
		const DescriptorUpdateTemplate<G>& setTemplate,
		DescriptorSetArrayList<G>& setArrayList);
	void internalPushDescriptorSet(
		CommandBufferHandle<G> cmd,
		const BindingsData<G>& bindingsData,
		const DescriptorUpdateTemplate<G>& setTemplate) const;

	static void internalUpdateDescriptorSetTemplate(
		const BindingsMap<G>& bindingsMap, DescriptorUpdateTemplate<G>& setTemplate);

	uint64_t internalCalculateHashKey() const;
	PipelineHandle<G> internalCreateGraphicsPipeline(uint64_t hashKey);
	PipelineHandle<G> internalGetPipeline();
	const PipelineLayout<G>& internalGetLayout();

	file::Object<PipelineConfiguration<G>, file::AccessMode::ReadWrite, true> myConfig;

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
