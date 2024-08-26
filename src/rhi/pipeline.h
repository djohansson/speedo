#pragma once

#include "descriptorset.h"
#include "device.h"
#include "image.h"
#include "model.h"
#include "queue.h"
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
	~PipelineLayout() override;

	PipelineLayout& operator=(PipelineLayout&& other) noexcept;
	[[nodiscard]] operator auto() const noexcept { return myLayout; }//NOLINT(google-explicit-constructor)
	[[nodiscard]] bool operator==(const PipelineLayout& other) const noexcept { return myLayout == other; }
	[[nodiscard]] bool operator<(const PipelineLayout& other) const noexcept { return myLayout < other; }

	void Swap(PipelineLayout& rhs) noexcept;
	friend void Swap(PipelineLayout& lhs, PipelineLayout& rhs) noexcept { lhs.Swap(rhs); }

	[[nodiscard]] const auto& GetShaderModules() const noexcept { return myShaderModules; }
	[[nodiscard]] const auto& GetDescriptorSetLayouts() const noexcept { return myDescriptorSetLayouts; }
	[[nodiscard]] const DescriptorSetLayout<G>& GetDescriptorSetLayout(uint32_t set) const noexcept;

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

	using PipelineLayoutSetType = UnorderedSet<
		PipelineLayout<G>,
		HandleHash<PipelineLayout<G>,
		PipelineLayoutHandle<G>>>;

	using DescriptorMapType = UnorderedMap<
		DescriptorSetLayoutHandle<G>, // todo: monitor mem usage, and find good strategy for recycling memory and to what level we should cache this data after being consumed.
		DescriptorSetState<G>>;

public:
	explicit Pipeline(
		const std::shared_ptr<Device<G>>& device,
		PipelineConfiguration<G>&& defaultConfig = {});
	~Pipeline() override;

	[[nodiscard]] const auto& GetConfig() const noexcept { return myConfig; }
	[[nodiscard]] auto GetCache() const noexcept { return myCache; }
	[[nodiscard]] auto GetDescriptorPool() const noexcept { return myDescriptorPool; }
	[[nodiscard]] auto GetBindPoint() const noexcept { return myBindPoint; }
	[[nodiscard]] PipelineLayoutHandle<G> GetLayout() const noexcept;

	[[maybe_unused]] PipelineLayoutHandle<G> CreateLayout(const ShaderSet<G>& shaderSet);

	// "manual" api

	void BindPipeline(
		CommandBufferHandle<G> cmd,
		PipelineBindPoint<G> bindPoint,
		PipelineHandle<G> handle) const;

	void BindDescriptorSet(
		CommandBufferHandle<G> cmd,
		DescriptorSetHandle<G> handle,
		PipelineBindPoint<G> bindPoint,
		PipelineLayoutHandle<G> layoutHandle,
		uint32_t set,
		std::optional<uint32_t> bufferOffset = std::nullopt) const;

	// "auto" api

	[[maybe_unused]] PipelineHandle<G> BindPipelineAuto(CommandBufferHandle<G> cmd); // todo: make implicit and call internally whenever relevant state changes

	void BindLayoutAuto(PipelineLayoutHandle<G> layout, PipelineBindPoint<G> bindPoint);

	[[nodiscard]] TaskCreateInfo<void> BindDescriptorSetAuto(
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

	// todo: rewrite to use generic resource model
	[[nodiscard]] auto& GetRenderTarget() const noexcept
	{
		ASSERT(myRenderTarget);
		return *myRenderTarget;
	}
	void SetRenderTarget(const std::shared_ptr<RenderTarget<G>>& renderTarget);
	[[nodiscard]] std::shared_ptr<Model<G>> SetModel(const std::shared_ptr<Model<G>>& model);
	[[nodiscard]] auto& GetResources() noexcept { return myResources; }
	[[nodiscard]] const auto& GetResources() const noexcept { return myResources; }
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

	[[nodiscard]] uint64_t InternalCalculateHashKey() const;
	[[nodiscard]] PipelineHandle<G> InternalCreateGraphicsPipeline(uint64_t hashKey);
	[[nodiscard]] PipelineHandle<G> InternalCreateComputePipeline(uint64_t hashKey);
	[[nodiscard]] PipelineHandle<G> InternalGetPipeline();
	[[nodiscard]] auto InternalGetLayout() const noexcept { return myCurrentLayoutIt; }

	file::Object<PipelineConfiguration<G>, file::AccessMode::kReadWrite, true> myConfig;

	DescriptorMapType myDescriptorMap;
	
	// todo: should be handled differently to cater for multithread and explicit binds.
	DescriptorPoolHandle<G> myDescriptorPool{};

	// todo: move pipeline map & cache to its own class, and pass in reference to it.
	PipelineMapType myPipelineMap; 
	PipelineCacheHandle<G> myCache{};

	// auto api shared state
	PipelineBindPoint<G> myBindPoint{};
	std::shared_ptr<RenderTarget<G>> myRenderTarget;
	PipelineResourceView<G> myResources;
	PipelineLayoutSetType myLayouts;
	//UnorderedSet<RenderTarget<G>> renderTargets;
	typename PipelineLayoutSetType::iterator myCurrentLayoutIt{};
	// end auto api shared state

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
	} myGraphicsState{};

	struct ComputeState
	{
		PipelineShaderStageCreateInfo<G> shaderStage;
		ComputeLaunchParameters launchParameters;
	} myComputeState{};

	struct RayTracingState
	{
		// todo:
	} myRayTracingState{};
};

#include "pipeline.inl"
