#include <rhi/pipeline.h>
#include <rhi/rhi.h>
#include <rhi/vulkan/convert.h>
#include <rhi/vulkan/utils.h>

#include <format>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(PipelineLayout<kVk>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(PipelineLayout<kVk>);

template <>
void PipelineLayout<kVk>::Swap(PipelineLayout& rhs) noexcept
{
	DeviceObject<PipelineLayout<kVk>>::Swap(rhs);
	std::swap(myShaderModules, rhs.myShaderModules);
	std::swap(myDescriptorSetLayouts, rhs.myDescriptorSetLayouts);
	std::swap(myPushConstantRanges, rhs.myPushConstantRanges);
	std::swap(myLayout, rhs.myLayout);
}

template <>
PipelineLayout<kVk>& PipelineLayout<kVk>::operator=(PipelineLayout<kVk>&& other) noexcept
{
	Swap(other);
	return *this;
}

template <>
PipelineLayout<kVk>::PipelineLayout(PipelineLayout<kVk>&& other) noexcept
{
	Swap(other);
}

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

// a shader set layout as vulkan takes it: every binding partially bound, so the shaders may leave elements unwritten
static DescriptorSetLayoutCreateDesc<kVk> ToDescriptorSetLayoutCreateDesc(const ShaderSetLayout& layout)
{
	DescriptorSetLayoutCreateDesc<kVk> desc;
	for (const auto& binding : layout.bindings)
	{
		desc.bindings.push_back(DescriptorSetLayoutBinding<kVk>{
			.binding = binding.binding,
			.descriptorType = vk::ToVk(binding.type),
			.descriptorCount = binding.count,
			.stageFlags = vk::ToVk(binding.stages),
			.pImmutableSamplers = nullptr});
		desc.bindingFlags.push_back(VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);
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
	const ShaderSet& shaderSet)
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
		[&shaderSet, &desc]
		{
			DescriptorSetLayoutFlatMap<kVk> map;
			for (const auto& [set, shaderSetLayout] : shaderSet.layouts)
			{
				auto layout = pipeline::ToDescriptorSetLayoutCreateDesc(shaderSetLayout);
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
