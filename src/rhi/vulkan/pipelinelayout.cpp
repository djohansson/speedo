#include <rhi/pipeline.h>
#include <rhi/rhi.h>
#include <rhi/vulkan/convert.h>
#include <rhi/vulkan/utils.h>

#include <algorithm>
#include <format>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(PipelineLayout<kVk>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(PipelineLayout<kVk>);

// (Swap and the moves are every backend's, in pipeline.inl)

template <>
PipelineLayout<kVk>::PipelineLayout(
	CreateDescType&& desc,
	std::vector<ShaderModule<kVk>>&& shaderModules,
	DescriptorSetLayoutFlatMap<kVk>&& descriptorSetLayouts,
	PipelineLayoutHandle<kVk>&& layout)
	: DeviceObject<PipelineLayout<kVk>>(std::forward<CreateDescType>(desc))
	, myShaderModules(std::exchange(shaderModules, {}))
	, myDescriptorSetLayouts(std::exchange(descriptorSetLayouts, {}))
	, myPushConstantRanges(descriptorset::GetPushConstantRanges<kVk>(myDescriptorSetLayouts))
	, myLayout(std::forward<PipelineLayoutHandle<kVk>>(layout))
{}

template <>
PipelineLayout<kVk>::PipelineLayout(
	CreateDescType&& desc,
	std::vector<ShaderModule<kVk>>&& shaderModules,
	DescriptorSetLayoutFlatMap<kVk>&& descriptorSetLayouts)
	: PipelineLayout(
		  std::forward<CreateDescType>(desc),
		  std::forward<std::vector<ShaderModule<kVk>>>(shaderModules),
		  std::forward<DescriptorSetLayoutFlatMap<kVk>>(descriptorSetLayouts),
		  // read from desc, not GetDesc(): this runs before the delegated constructor has initialized the base
		  [&descriptorSetLayouts, &desc, this]
		  {
			  // todo: rewrite flatmap so that keys and vals are stored as separate arrays so that we dont have to make this conversion
			  auto handles = descriptorset::GetDescriptorSetLayoutHandles<kVk>(descriptorSetLayouts);
			  auto pushConstantRanges =
				  descriptorset::GetPushConstantRanges<kVk>(descriptorSetLayouts);

			  VkPipelineLayoutCreateInfo pipelineLayoutInfo{
				  .sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
			  pipelineLayoutInfo.setLayoutCount = handles.size();
			  pipelineLayoutInfo.pSetLayouts = handles.data();
			  pipelineLayoutInfo.pushConstantRangeCount = pushConstantRanges.size();
			  pipelineLayoutInfo.pPushConstantRanges = pushConstantRanges.data();

			  VkPipelineLayout layout;
			  VK_CHECK(vkCreatePipelineLayout(
				  desc.device,
				  &pipelineLayoutInfo,
				  &GetInstance().GetHostAllocationCallbacks(),
				  &layout));

			  Track(desc.device, VK_OBJECT_TYPE_PIPELINE_LAYOUT, layout, GetDebugName(desc));

			  return layout;
		  }())
{}

namespace pipeline
{

// a shader set layout as vulkan takes it: every binding partially bound, so the shaders may leave elements unwritten;
// a pushed set's (see Pipeline::CreateLayout) are not (a push descriptor set layout allows no binding flags), its
// descriptors all written at each push
static DescriptorSetLayoutCreateDesc<kVk> ToDescriptorSetLayoutCreateDesc(const ShaderSetLayout& layout, bool push)
{
	DescriptorSetLayoutCreateDesc<kVk> desc;
	if (push)
		desc.flags |= VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT;
	for (const auto& binding : layout.bindings)
	{
		desc.bindings.push_back(DescriptorSetLayoutBinding<kVk>{
			.binding = binding.binding,
			.descriptorType = vk::ToVk(binding.type),
			.descriptorCount = binding.count,
			.stageFlags = vk::ToVk(binding.stages),
			.pImmutableSamplers = nullptr});
		desc.bindingFlags.push_back(push ? 0 : VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);
		desc.variableNames.push_back(binding.name);
		desc.variableNameHashes.push_back(binding.nameHash);
	}
	if (const auto& pushConstants = layout.pushConstants)
		desc.pushConstantRange =
			PushConstantRange<kVk>{.stageFlags=vk::ToVk(pushConstants->stages), .offset=pushConstants->offset, .size=pushConstants->size};
	return desc;
}

} // namespace pipeline

template <>
PipelineLayout<kVk>::PipelineLayout(
	CreateDescType&& desc,
	const ShaderSet& shaderSet,
	std::span<const uint32_t> pushDescriptorSets)
	: PipelineLayout(
		std::forward<CreateDescType>(desc),
		// read from desc, not GetDesc(): this runs before the delegated constructor has initialized the base
		[&shaderSet, &desc, this]
		{
			std::vector<ShaderModule<kVk>> shaderModules;
			shaderModules.reserve(shaderSet.shaders.size());
			for (auto shader : shaderSet.shaders)
				shaderModules.emplace_back(
					ShaderModuleCreateDesc<kVk>{
						SuperType::CreateDeviceObjectCreateDesc("ShaderModule", desc.device),
						shader,
					});

			return shaderModules;
		}(),
		[&shaderSet, &desc, pushDescriptorSets]
		{
			DescriptorSetLayoutFlatMap<kVk> map;
			for (const auto& [set, shaderSetLayout] : shaderSet.layouts)
			{
				bool push = std::ranges::find(pushDescriptorSets, set) != pushDescriptorSets.end();
				auto layout = pipeline::ToDescriptorSetLayoutCreateDesc(shaderSetLayout, push);
				layout.instance = desc.instance;
				layout.device = desc.device;
				layout.uuid = uuids::NewUuid();
				layout.name = std::format("{} DescriptorSetLayout {}", GetDebugName(desc), set);
				map.emplace(set, DescriptorSetLayout<kVk>(std::move(layout)));
			}

			return map;
		}())
{}

template <>
PipelineLayout<kVk>::~PipelineLayout()
{
	if (IsValid())
	{
		Untrack(VK_OBJECT_TYPE_PIPELINE_LAYOUT, myLayout);
		vkDestroyPipelineLayout(
			GetDevice(),
			myLayout,
			&GetInstance().GetHostAllocationCallbacks());
	}
}

} // namespace rhi
