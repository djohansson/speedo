// the Metal backend's pipeline layouts: the entry points' libraries, the sets' argument buffer layouts, and the push
// constants (no Metal object: the handle is the backend's, see backend.h)
#include <rhi/metal/backend.h>

#include <rhi/pipeline.h>
#include <rhi/rhi.h>

#include <core/assert.h>

#include <algorithm>
#include <format>
#include <utility>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(PipelineLayout<kMetal>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(PipelineLayout<kMetal>);

template <>
PipelineLayout<kMetal>::PipelineLayout(
	CreateDescType&& desc,
	std::vector<ShaderModule<kMetal>>&& shaderModules,
	DescriptorSetLayoutFlatMap<kMetal>&& descriptorSetLayouts,
	PipelineLayoutHandle<kMetal>&& layout)
	: DeviceObject<PipelineLayout<kMetal>>(std::forward<CreateDescType>(desc))
	, myShaderModules(std::exchange(shaderModules, {}))
	, myDescriptorSetLayouts(std::exchange(descriptorSetLayouts, {}))
	, myPushConstantRanges(descriptorset::GetPushConstantRanges<kMetal>(myDescriptorSetLayouts))
	, myLayout(std::forward<PipelineLayoutHandle<kMetal>>(layout))
{}

template <>
PipelineLayout<kMetal>::PipelineLayout(
	CreateDescType&& desc,
	std::vector<ShaderModule<kMetal>>&& shaderModules,
	DescriptorSetLayoutFlatMap<kMetal>&& descriptorSetLayouts)
	: PipelineLayout(
		std::forward<CreateDescType>(desc),
		std::forward<std::vector<ShaderModule<kMetal>>>(shaderModules),
		std::forward<DescriptorSetLayoutFlatMap<kMetal>>(descriptorSetLayouts),
		[&descriptorSetLayouts]
		{
			auto* layout = new metal::PipelineLayoutObject{};
			for (const auto& range : descriptorset::GetPushConstantRanges<kMetal>(descriptorSetLayouts))
				layout->pushConstantSize = std::max(layout->pushConstantSize, range.offset + range.size);
			return layout;
		}())
{}

namespace pipeline
{

// a shader set layout as the backend takes it: a pushed set's (see Pipeline::CreateLayout) flagged
static DescriptorSetLayoutCreateDesc<kMetal> ToDescriptorSetLayoutCreateDesc(const ShaderSetLayout& layout, bool push)
{
	DescriptorSetLayoutCreateDesc<kMetal> desc;
	if (push)
		desc.flags |= metal::kDescriptorSetLayoutPush;
	for (const auto& binding : layout.bindings)
	{
		desc.bindings.push_back({.binding = binding.binding, .descriptorType = binding.type, .descriptorCount = binding.count});
		desc.bindingFlags.push_back(0);
		desc.variableNames.push_back(binding.name);
		desc.variableNameHashes.push_back(binding.nameHash);
	}
	if (const auto& pushConstants = layout.pushConstants)
		desc.pushConstantRange =
			PushConstantRange<kMetal>{.stages = pushConstants->stages, .offset = pushConstants->offset, .size = pushConstants->size};
	return desc;
}

} // namespace pipeline

template <>
PipelineLayout<kMetal>::PipelineLayout(CreateDescType&& desc, const ShaderSet& shaderSet, std::span<const uint32_t> pushDescriptorSets)
	: PipelineLayout(
		std::forward<CreateDescType>(desc),
		// read from desc, not GetDesc(): this runs before the delegated constructor has initialized the base
		[&shaderSet, &desc, this]
		{
			std::vector<ShaderModule<kMetal>> shaderModules;
			shaderModules.reserve(shaderSet.shaders.size());
			for (const auto& shader : shaderSet.shaders)
				shaderModules.emplace_back(
					ShaderModuleCreateDesc<kMetal>{
						SuperType::CreateDeviceObjectCreateDesc("ShaderModule", desc.device),
						shader,
					});
			return shaderModules;
		}(),
		[&shaderSet, &desc, pushDescriptorSets]
		{
			DescriptorSetLayoutFlatMap<kMetal> map;
			for (const auto& [set, shaderSetLayout] : shaderSet.layouts)
			{
				bool push = std::ranges::find(pushDescriptorSets, set) != pushDescriptorSets.end();
				auto layout = pipeline::ToDescriptorSetLayoutCreateDesc(shaderSetLayout, push);
				layout.instance = desc.instance;
				layout.device = desc.device;
				layout.uuid = uuids::NewUuid();
				layout.name = std::format("{} DescriptorSetLayout {}", GetDebugName(desc), set);
				map.emplace(set, DescriptorSetLayout<kMetal>(std::move(layout)));
			}
			return map;
		}())
{}

template <>
PipelineLayout<kMetal>::~PipelineLayout()
{
	delete myLayout;
}

} // namespace rhi
