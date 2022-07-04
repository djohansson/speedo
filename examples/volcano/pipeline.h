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
class PipelineLayout : public DeviceObject<B>
{
public:
	constexpr PipelineLayout() noexcept = default;
	PipelineLayout(PipelineLayout<B>&& other) noexcept;
	PipelineLayout(
		const std::shared_ptr<DeviceContext<B>>& deviceContext, const ShaderSet<B>& shaderSet);
	~PipelineLayout();

	PipelineLayout& operator=(PipelineLayout&& other) noexcept;
	operator auto() const noexcept { return myLayout; }
	bool operator==(const PipelineLayout& other) const noexcept { return myLayout == other; }
	bool operator<(const PipelineLayout& other) const noexcept { return myLayout < other; }

	void swap(PipelineLayout& rhs) noexcept;
	friend void swap(PipelineLayout& lhs, PipelineLayout& rhs) noexcept { lhs.swap(rhs); }

	const auto& getShaderModules() const noexcept { return myShaderModules; }
	const auto& getDescriptorSetLayouts() const noexcept { return myDescriptorSetLayouts; }
	const DescriptorSetLayout<B>& getDescriptorSetLayout(uint32_t set) const noexcept;

private:
	PipelineLayout( // takes ownership over provided handles
		const std::shared_ptr<DeviceContext<B>>& deviceContext,
		std::vector<ShaderModule<B>>&& shaderModules,
		DescriptorSetLayoutFlatMap<B>&& descriptorSetLayouts);
	PipelineLayout( // takes ownership over provided handles
		const std::shared_ptr<DeviceContext<B>>& deviceContext,
		std::vector<ShaderModule<B>>&& shaderModules,
		DescriptorSetLayoutFlatMap<B>&& descriptorSetLayouts,
		PipelineLayoutHandle<B>&& layout);

	std::vector<ShaderModule<B>> myShaderModules;
	DescriptorSetLayoutFlatMap<B> myDescriptorSetLayouts;
	PipelineLayoutHandle<B> myLayout{};
};

template <GraphicsBackend B>
struct PipelineResourceView
{
	// begin temp
	std::shared_ptr<Model<B>> model;
	std::shared_ptr<Image<B>> black;
	std::shared_ptr<ImageView<B>> blackImageView;
	std::shared_ptr<Image<B>> image;
	std::shared_ptr<ImageView<B>> imageView;
	std::shared_ptr<SamplerVector<B>> samplers;
	// end temp
};

template <GraphicsBackend B>
struct PipelineCacheHeader
{};

template <GraphicsBackend B>
struct PipelineConfiguration
{
	std::filesystem::path cachePath;
};

// todo: create single-thread / multi-thread interface:
//         * fork() pushes/copies/pops thread-specific state on stack
//         * descriptor pools created in groups for each thread instance
//         * pipeline map/cache shared across thread instances
//         * descriptor data shared across thread instances (if possible. avoids excessive copying...)
template <GraphicsBackend B>
class PipelineContext : public DeviceObject<B>
{
	using PipelineMapType = UnorderedMap<
		uint64_t, // pipeline object key (pipeline layout + gfx/compute/raytrace state)
		CopyableAtomic<PipelineHandle<B>>,
		IdentityHash<uint64_t>>;

	using DescriptorMapType = UnorderedMap<
		DescriptorSetLayoutHandle<
			B>, // todo: hash DescriptorSetData into this key? monitor mem useage, and find good strategy for recycling memory and to what level we should cache this data after being consumed.
		DescriptorSetState<B>>;

public:
	PipelineContext(
		const std::shared_ptr<DeviceContext<B>>& deviceContext,
		PipelineConfiguration<B>&& defaultConfig = {});
	~PipelineContext();

	const auto& getConfig() const noexcept { return myConfig; }
	auto getCache() const noexcept { return myCache; }
	auto getDescriptorPool() const noexcept { return myDescriptorPool; }
	auto getBindPoint() const noexcept { return myBindPoint; }

	// "manual" api

	void bindPipeline(
		CommandBufferHandle<B> cmd, PipelineBindPoint<B> bindPoint, PipelineHandle<B> handle) const;

	void bindDescriptorSet(
		CommandBufferHandle<B> cmd,
		DescriptorSetHandle<B> handle,
		PipelineBindPoint<B> bindPoint,
		PipelineLayoutHandle<B> layoutHandle,
		uint32_t set,
		std::optional<uint32_t> bufferOffset = std::nullopt) const;

	// "auto" api

	void bindPipelineAuto(CommandBufferHandle<B> cmd);

	void bindDescriptorSetAuto(
		CommandBufferHandle<B> cmd,
		uint32_t set,
		std::optional<uint32_t> bufferOffset = std::nullopt);

	template <typename T>
	void setDescriptorData(
		uint64_t shaderVariableNameHash, const DescriptorSetLayout<B>& layout, T&& data);

	template <typename T>
	void setDescriptorData(std::string_view shaderVariableName, T&& data, uint32_t set);

	template <typename T>
	void setDescriptorData(
		uint64_t shaderVariableNameHash,
		const DescriptorSetLayout<B>& layout,
		const std::vector<T>& data);

	template <typename T>
	void setDescriptorData(
		std::string_view shaderVariableName, const std::vector<T>& data, uint32_t set);

	template <typename T>
	void setDescriptorData(
		uint64_t shaderVariableNameHash,
		const DescriptorSetLayout<B>& layout,
		T&& data,
		uint32_t index);

	template <typename T>
	void
	setDescriptorData(std::string_view shaderVariableName, T&& data, uint32_t set, uint32_t index);

	// temp
	const auto& getLayout() const noexcept
	{
		assert(myLayout);
		return *myLayout;
	}
	auto& getRenderTarget() const noexcept
	{
		assert(myRenderTarget);
		return *myRenderTarget;
	}

	void
	setLayout(const std::shared_ptr<PipelineLayout<B>>& layout, PipelineBindPoint<B> bindPoint);
	void setRenderTarget(const std::shared_ptr<RenderTarget<B>>& renderTarget);
	void setModel(const std::shared_ptr<Model<B>>&
					  model); // todo: rewrite to use generic draw call structures / buffers

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
		const DescriptorSetLayout<B>& setLayout,
		const BindingsData<B>& bindingsData,
		const DescriptorUpdateTemplate<B>& setTemplate,
		DescriptorSetArrayList<B>& setArrayList);
	void internalPushDescriptorSet(
		CommandBufferHandle<B> cmd,
		const BindingsData<B>& bindingsData,
		const DescriptorUpdateTemplate<B>& setTemplate) const;

	static void internalUpdateDescriptorSetTemplate(
		const BindingsMap<B>& bindingsMap, DescriptorUpdateTemplate<B>& setTemplate);

	uint64_t internalCalculateHashKey() const;
	PipelineHandle<B> internalCreateGraphicsPipeline(uint64_t hashKey);
	PipelineHandle<B> internalGetPipeline();

	AutoSaveJSONFileObject<PipelineConfiguration<B>> myConfig;

	DescriptorMapType myDescriptorMap;
	
	// todo: should be handled differently to cater for multithread and explicit binds.
	DescriptorPoolHandle<B> myDescriptorPool{};

	// todo: move pipeline map & cache to its own class, and pass in reference to it.
	PipelineMapType myPipelineMap; 
	PipelineCacheHandle<B> myCache{};

	// shared state
	PipelineBindPoint<B> myBindPoint{};
	std::shared_ptr<PipelineLayout<B>> myLayout;
	std::shared_ptr<RenderTarget<B>> myRenderTarget;
	// end shared state

	struct GraphicsState
	{
		std::vector<PipelineShaderStageCreateInfo<B>> shaderStages;
		uint32_t shaderStageFlags{};
		PipelineVertexInputStateCreateInfo<B> vertexInput{};
		PipelineInputAssemblyStateCreateInfo<B> inputAssembly{};
		std::vector<Viewport<B>> viewports;
		std::vector<Rect2D<B>> scissorRects;
		PipelineViewportStateCreateInfo<B> viewport{};
		PipelineRasterizationStateCreateInfo<B> rasterization{};
		PipelineMultisampleStateCreateInfo<B> multisample{};
		PipelineDepthStencilStateCreateInfo<B> depthStencil{};
		std::vector<PipelineColorBlendAttachmentState<B>> colorBlendAttachments{};
		PipelineColorBlendStateCreateInfo<B> colorBlend{};
		std::vector<DynamicState<B>> dynamicStateDescs;
		PipelineDynamicStateCreateInfo<B> dynamicState{};
		// temp
		PipelineResourceView<B> resources;
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

#include "pipeline-vulkan.inl"
#include "pipeline.inl"
