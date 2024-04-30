#include "../shader.h"

#include "utils.h"

#include <iostream>

#include <xxhash.h>

namespace shader
{

template <>
ShaderStageFlagBits<Vk> getStageFlags<Vk>(SlangStage stage)
{
	switch (stage)
	{
	case SLANG_STAGE_VERTEX:
		return VK_SHADER_STAGE_VERTEX_BIT;
	case SLANG_STAGE_FRAGMENT:
		return VK_SHADER_STAGE_FRAGMENT_BIT;
	case SLANG_STAGE_HULL:
		return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
	case SLANG_STAGE_DOMAIN:
		return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
	case SLANG_STAGE_GEOMETRY:
		return VK_SHADER_STAGE_GEOMETRY_BIT;
	case SLANG_STAGE_COMPUTE:
		return VK_SHADER_STAGE_COMPUTE_BIT;
	case SLANG_STAGE_RAY_GENERATION:
		return VK_SHADER_STAGE_RAYGEN_BIT_KHR;
	case SLANG_STAGE_INTERSECTION:
		return VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
	case SLANG_STAGE_ANY_HIT:
		return VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
	case SLANG_STAGE_CLOSEST_HIT:
		return VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
	case SLANG_STAGE_MISS:
		return VK_SHADER_STAGE_MISS_BIT_KHR;
	case SLANG_STAGE_CALLABLE:
		return VK_SHADER_STAGE_CALLABLE_BIT_KHR;
	case SLANG_STAGE_NONE:
		return VK_SHADER_STAGE_ALL; // todo: fix when slang handles this case.
	default:
		assert(false); // please implement me!
	}

	return ShaderStageFlagBits<Vk>{};
}

template <>
DescriptorType<Vk> getDescriptorType<Vk>(
	slang::TypeReflection::Kind kind, SlangResourceShape shape, SlangResourceAccess access)
{
	auto type = DescriptorType<Vk>{};

	switch (kind)
	{
	case slang::TypeReflection::Kind::ConstantBuffer:
		type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		break;
	case slang::TypeReflection::Kind::TextureBuffer:
		type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER; //?
		break;
	case slang::TypeReflection::Kind::ShaderStorageBuffer:
		type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		break;
	case slang::TypeReflection::Kind::Resource: {
		switch (shape & SLANG_RESOURCE_BASE_SHAPE_MASK)
		{
		case SLANG_TEXTURE_1D:
		case SLANG_TEXTURE_2D:
		case SLANG_TEXTURE_3D:
		case SLANG_TEXTURE_CUBE:
			type =
				(access == SLANG_RESOURCE_ACCESS_READ_WRITE ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
															: VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
			break;
		case SLANG_STRUCTURED_BUFFER:
			type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			break;
		default:
			assert(false); // please implement me!
			break;
		}
		break;
	}
	case slang::TypeReflection::Kind::SamplerState:
		type = VK_DESCRIPTOR_TYPE_SAMPLER;
		break;
	case slang::TypeReflection::Kind::ParameterBlock:
		type = VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT;
		break;
	case slang::TypeReflection::Kind::Array:
	case slang::TypeReflection::Kind::Matrix:
	case slang::TypeReflection::Kind::Vector:
	case slang::TypeReflection::Kind::Scalar:
	case slang::TypeReflection::Kind::None:
	case slang::TypeReflection::Kind::Struct:
	case slang::TypeReflection::Kind::GenericTypeParameter:
	case slang::TypeReflection::Kind::Interface:
	case slang::TypeReflection::Kind::OutputStream:
	case slang::TypeReflection::Kind::Specialized:
	default:
		assert(false); // please implement me!
		break;
	}

	return type;
}

void addBinding(
	unsigned bindingIndex,
	unsigned bindingSpace,
	slang::TypeLayoutReflection* typeLayout,
	bool usePushConstant,
	size_t sizeBytes,
	SlangStage stage,
	std::string_view name,
	std::map<uint32_t, DescriptorSetLayoutCreateDesc<Vk>>& layouts)
{
	assert(typeLayout);

	auto& layout = layouts[bindingSpace];

	bool isArray = typeLayout->isArray();
	auto descriptorCount = isArray ? typeLayout->getElementCount() : 1;
	auto kind = typeLayout->getKind();
	auto shape = typeLayout->getType()->getResourceShape();
	auto access = typeLayout->getType()->getResourceAccess();

	if (kind == slang::TypeReflection::Kind::Array)
	{
		auto* elementTypeLayout = typeLayout->getElementTypeLayout();
		kind = elementTypeLayout->getKind();
		shape = elementTypeLayout->getType()->getResourceShape();
		access = elementTypeLayout->getType()->getResourceAccess();
	}

	auto descriptorType = shader::getDescriptorType<Vk>(kind, shape, access);

	//auto isUniformDynamic = descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	auto isInlineUniformBlock = descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT;

	DescriptorSetLayoutBinding<Vk> slot;
	slot.binding = bindingIndex;
	slot.descriptorType = descriptorType;
	slot.descriptorCount = isInlineUniformBlock ? sizeBytes : descriptorCount;
	slot.stageFlags = shader::getStageFlags<Vk>(stage);
	slot.pImmutableSamplers = nullptr;

	layout.bindings.push_back(slot);
	layout.bindingFlags.push_back(
		// VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
		// VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT |
		// VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT |
		VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);
	layout.variableNames.push_back(name.data());
	layout.variableNameHashes.push_back(XXH3_64bits(name.data(), name.size()));

	// todo: immutable samplers
	//layout.immutableSamplers.push_back(SamplerCreateInfo<Vk>{});

	// todo: push descriptors
	constexpr bool usePushDescriptor = false;
	if (usePushDescriptor)
	{
		//assert(!isUniformDynamic);
		assert(!isInlineUniformBlock);
		layout.flags |= VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
	}

	if (usePushConstant)
	{
		assert(!layout.pushConstantRange);
		assert(sizeBytes > 0);
		//assert(!isUniformDynamic);
		assert(!isInlineUniformBlock);
		layout.pushConstantRange =
			PushConstantRange<Vk>{slot.stageFlags, 0, static_cast<uint32_t>(sizeBytes)};
	}

	std::cout << "ADD BINDING \"" << name << "\": Set: " << bindingSpace
			  << ", Binding: " << slot.binding << ", Count: " << descriptorCount
			  << ", Size: " << sizeBytes << '\n';
}

template <>
uint32_t createLayoutBindings<Vk>(
	slang::VariableLayoutReflection* parameter,
	const std::vector<uint32_t>& genericParameterIndices,
	std::map<uint32_t, DescriptorSetLayoutCreateDesc<Vk>>& layouts,
	const unsigned* parentSpace,
	const char* parentName)
{
	assert(parameter);
	auto space = parameter->getBindingSpace();
	auto index = parameter->getBindingIndex();
	auto name = std::string(parameter->getName());
	auto stage = parameter->getStage();
	auto categoryCount = parameter->getCategoryCount();
	auto* typeLayout = parameter->getTypeLayout();
	assert(typeLayout);
	auto arrayElementCount = typeLayout->getElementCount();
	auto fieldCount = typeLayout->getFieldCount();
	auto* elementTypeLayout = typeLayout->getElementTypeLayout();
	//assert(elementTypeLayout);
	auto elementFieldCount = elementTypeLayout ? elementTypeLayout->getFieldCount() : 0;

	auto category = parameter->getCategory();
	auto typeName = typeLayout->getName();
	assert(typeName);
	auto kind = typeLayout->getKind();
	auto type = typeLayout->getType();
	assert(type);
	auto userAttributeCount = type->getUserAttributeCount();
	auto elementKind = elementTypeLayout ? elementTypeLayout->getKind() : slang::TypeReflection::Kind::None;
	auto genericParamIndex = elementTypeLayout ? elementTypeLayout->getGenericParamIndex() : 0;

	std::string fullName;
	if (parentName)
	{
		fullName.append(parentName);
		fullName.append(".");
	}
	fullName.append(name);

	auto bindingSpace = (parentSpace ? *parentSpace : space);

	uint32_t uniformsTotalSize = 0;

	for (auto categoryIndex = 0; categoryIndex < categoryCount; categoryIndex++)
	{
		auto subCategory = static_cast<SlangParameterCategory>(parameter->getCategoryByIndex(categoryIndex));
		auto spaceForCategory = parameter->getBindingSpace(subCategory);
		auto elementSize = elementTypeLayout ? elementTypeLayout->getSize(subCategory) : 0;
		auto elementAlignment = elementTypeLayout ? elementTypeLayout->getAlignment(subCategory) : 0;
		auto size = typeLayout->getSize(subCategory);
		auto alignment = typeLayout->getAlignment(subCategory);

		auto offsetForCategory = parameter->getOffset(subCategory);
		auto elementStride = elementTypeLayout ? elementTypeLayout->getElementStride(subCategory) : 0;

		std::cout << "DEBUG: name: " << name << ", fullName: " << fullName << ", space: " << space
				  << ", parent space: "
				  << (parentSpace ? std::to_string(*parentSpace) : "(nullptr)")
				  << ", index: " << index << ", stage: " << stage << ", kind: " << (int)kind
				  << ", typeName: " << (typeName ? typeName : "(nullptr)")
				  << ", userAttributeCount: " << userAttributeCount;

		std::cout << ", arrayElementCount: " << arrayElementCount << ", fieldCount: " << fieldCount;

		std::cout << ", category: " << category << ", subCategory: " << subCategory
				  << ", spaceForCategory: " << spaceForCategory
				  << ", offsetForCategory: " << offsetForCategory
				  << ", elementStride: " << elementStride << ", elementSize: " << elementSize
				  << ", elementAlignment: " << elementAlignment
				  << ", elementKind: " << (int)elementKind
				  << ", elementFieldCount: " << elementFieldCount << ", size: " << size
				  << ", alignment: " << alignment << ", genericParamIndex: " << genericParamIndex;

		std::cout << '\n';

		if (subCategory == SLANG_PARAMETER_CATEGORY_REGISTER_SPACE)
		{
			bindingSpace = spaceForCategory;
		}
		else if (subCategory == SLANG_PARAMETER_CATEGORY_UNIFORM)
		{
			uniformsTotalSize += size;
		}
	}

	for (auto elementFieldIndex = 0; elementFieldIndex < elementFieldCount; elementFieldIndex++)
	{
		auto elementField = elementTypeLayout ? elementTypeLayout->getFieldByIndex(elementFieldIndex) : nullptr;
		//assert(elementField);
		auto elementFieldType = elementField ? elementField->getTypeLayout() : nullptr;
		//assert(elementFieldType);
		auto count = elementFieldType && elementFieldType->isArray() ? elementFieldType->getFieldCount() : 1;
		uniformsTotalSize +=
			count *
			createLayoutBindings<Vk>(
				elementField, genericParameterIndices, layouts, &bindingSpace, fullName.c_str());
	}

	for (auto categoryIndex = 0; categoryIndex < categoryCount; categoryIndex++)
	{
		auto subCategory = parameter->getCategoryByIndex(categoryIndex);

		if (subCategory == slang::ParameterCategory::DescriptorTableSlot ||
			subCategory == slang::ParameterCategory::PushConstantBuffer)
		{
			addBinding(
				index,
				bindingSpace,
				typeLayout,
				subCategory == slang::ParameterCategory::PushConstantBuffer,
				uniformsTotalSize,
				stage,
				fullName,
				layouts);
		}
	}

	return uniformsTotalSize;
}

} // namespace shader

template <>
ShaderModule<Vk>::ShaderModule(
	const std::shared_ptr<Device<Vk>>& device,
	ShaderModuleHandle<Vk>&& shaderModule,
	const EntryPoint<Vk>& entryPoint)
	: DeviceObject(
		  device,
		  {std::get<0>(entryPoint).c_str()},
		  1,
		  VK_OBJECT_TYPE_SHADER_MODULE,
		  reinterpret_cast<uint64_t*>(&shaderModule))
	, myShaderModule(std::forward<ShaderModuleHandle<Vk>>(shaderModule))
	, myEntryPoint(entryPoint)
{}

template <>
ShaderModule<Vk>::ShaderModule(
	const std::shared_ptr<Device<Vk>>& device, const Shader<Vk>& shader)
	: ShaderModule<Vk>(
		  device,
		  createShaderModule(
			  *device,
			  &device->getInstance()->getHostAllocationCallbacks(),
			  std::get<0>(shader).size(),
			  reinterpret_cast<const uint32_t*>(std::get<0>(shader).data())),
		  std::get<1>(shader))
{}

template <>
ShaderModule<Vk>::ShaderModule(ShaderModule&& other) noexcept
	: DeviceObject(std::forward<ShaderModule>(other))
	, myShaderModule(std::exchange(other.myShaderModule, {}))
	, myEntryPoint(std::exchange(other.myEntryPoint, {}))
{}

template <>
ShaderModule<Vk>::~ShaderModule()
{
	if (myShaderModule)
		vkDestroyShaderModule(
			*getDevice(),
			myShaderModule,
			&getDevice()->getInstance()->getHostAllocationCallbacks());
}

template <>
ShaderModule<Vk>& ShaderModule<Vk>::operator=(ShaderModule&& other) noexcept
{
	DeviceObject::operator=(std::forward<ShaderModule>(other));
	myShaderModule = std::exchange(other.myShaderModule, {});
	myEntryPoint = std::exchange(other.myEntryPoint, {});
	return *this;
}

template <>
void ShaderModule<Vk>::swap(ShaderModule& rhs) noexcept
{
	DeviceObject::swap(rhs);
	std::swap(myShaderModule, rhs.myShaderModule);
	std::swap(myEntryPoint, rhs.myEntryPoint);
}
