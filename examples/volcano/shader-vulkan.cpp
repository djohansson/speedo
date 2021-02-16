#include "shader.h"
#include "vk-utils.h"

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
DescriptorType<Vk> getDescriptorType<Vk>(slang::TypeReflection::Kind kind, SlangResourceShape shape)
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
	case slang::TypeReflection::Kind::Resource:
	{
		switch (shape & SLANG_RESOURCE_BASE_SHAPE_MASK)
		{
			case SLANG_TEXTURE_2D:
			case SLANG_TEXTURE_1D:
			case SLANG_TEXTURE_3D:
			case SLANG_TEXTURE_CUBE:
				type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
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
	std::unordered_map<uint32_t, DescriptorSetLayoutCreateDesc<Vk>>& layouts)
{
	auto& layout = layouts[bindingSpace];

	bool isArray = typeLayout->isArray();
	auto descriptorCount = isArray ? typeLayout->getElementCount() : 1;
	auto kind = typeLayout->getKind();
	auto shape = typeLayout->getType()->getResourceShape();

	if (kind == slang::TypeReflection::Kind::Array)
	{
		auto* elementTypeLayout = typeLayout->getElementTypeLayout();
		kind = elementTypeLayout->getKind();
		shape = elementTypeLayout->getType()->getResourceShape();
	}

	auto descriptorType = shader::getDescriptorType<Vk>(kind, shape);

	auto isUniformDynamic = descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	auto isInlineUniformBlock = descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT;

	DescriptorSetLayoutBinding<Vk> slot;
	slot.binding = bindingIndex;
	slot.descriptorType = descriptorType;
	slot.descriptorCount = isInlineUniformBlock ? sizeBytes : descriptorCount;
	slot.stageFlags = shader::getStageFlags<Vk>(stage);
	slot.pImmutableSamplers = nullptr;

	layout.bindings.push_back(slot);
	layout.variableNames.push_back(name.data());
	layout.variableNameHashes.push_back(XXH3_64bits(name.data(), name.size()));
	
	// todo: immutable samplers
	//layout.immutableSamplers.push_back(SamplerCreateInfo<Vk>{});

	// todo: push descriptors
	// if (usePushDescriptor)
	// {
	// 	assert(!isUniformDynamic);
	// 	assert(!isInlineUniformBlock);
	// 	layout.flags |= VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
	// }

	if (usePushConstant)
	{
		assert(!layout.pushConstantRange);
		assert(sizeBytes > 0);
		assert(!isUniformDynamic);
		assert(!isInlineUniformBlock);
		layout.pushConstantRange = PushConstantRange<Vk>{slot.stageFlags, 0, static_cast<uint32_t>(sizeBytes)};
	}

	std::cout << "ADD BINDING \"" << name <<
		"\": Set: " << bindingSpace <<
		", Binding: " << slot.binding <<
		", Count: " << descriptorCount <<
		", Size: " << sizeBytes << std::endl;
};

template <>
uint32_t createLayoutBindings<Vk>(
    slang::VariableLayoutReflection* parameter,
	const std::vector<uint32_t>& genericParameterIndices,
    std::unordered_map<uint32_t, DescriptorSetLayoutCreateDesc<Vk>>& layouts,
	const unsigned* parentSpace,
	const char* parentName)
{
	auto space = parameter->getBindingSpace();
	auto index = parameter->getBindingIndex();
	auto name = std::string(parameter->getName());
	auto stage = parameter->getStage();
	auto categoryCount = parameter->getCategoryCount();
	auto* typeLayout = parameter->getTypeLayout();
	auto arrayElementCount = typeLayout->getElementCount();
	auto fieldCount = typeLayout->getFieldCount();
	auto* elementTypeLayout = typeLayout->getElementTypeLayout();
	auto elementFieldCount = elementTypeLayout->getFieldCount();
	
	auto category = parameter->getCategory();
	auto typeName = typeLayout->getName();
	auto kind = typeLayout->getKind();
	auto type = typeLayout->getType();
	auto userAttributeCount = type->getUserAttributeCount();
	auto elementKind = elementTypeLayout->getKind();
	auto genericParamIndex = elementTypeLayout->getGenericParamIndex();

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
		auto subCategory = parameter->getCategoryByIndex(categoryIndex);
		auto spaceForCategory = parameter->getBindingSpace(subCategory);
		auto elementSize = elementTypeLayout->getSize(subCategory);
		auto elementAlignment = elementTypeLayout->getAlignment(subCategory);
		auto size = typeLayout->getSize(subCategory);
		auto alignment = typeLayout->getAlignment(subCategory);

		auto offsetForCategory = parameter->getOffset(subCategory);
		auto elementStride = elementTypeLayout->getElementStride(subCategory);		

		std::cout << "DEBUG: name: " << name
			<< ", fullName: " << fullName
			<< ", space: " << space
			<< ", parent space: " << (parentSpace ? std::to_string(*parentSpace) : "(nullptr)")
			<< ", index: " << index
			<< ", stage: " << stage
			<< ", kind: " << (int)kind
			<< ", typeName: " << (typeName ? typeName : "(nullptr)")
			<< ", userAttributeCount: " << userAttributeCount;
	
		std::cout << ", arrayElementCount: " << arrayElementCount
			<< ", fieldCount: " << fieldCount;

		std::cout << ", category: " << category
			<< ", subCategory: " << subCategory
			<< ", spaceForCategory: " << spaceForCategory
			<< ", offsetForCategory: " << offsetForCategory
			<< ", elementStride: " << elementStride
			<< ", elementSize: " << elementSize
			<< ", elementAlignment: " << elementAlignment
			<< ", elementKind: " << (int)elementKind
			<< ", elementFieldCount: " << elementFieldCount
			<< ", size: " << size
			<< ", alignment: " << alignment
			<< ", genericParamIndex: " << genericParamIndex;

		std::cout << std::endl;

		if (subCategory == slang::ParameterCategory::RegisterSpace)
		{
			bindingSpace = spaceForCategory;
		}
		else if (subCategory == slang::ParameterCategory::Uniform)
		{
			uniformsTotalSize += size;
		}
	}

	for (auto elementFieldIndex = 0; elementFieldIndex < elementFieldCount; elementFieldIndex++)
	{
		auto elementField = elementTypeLayout->getFieldByIndex(elementFieldIndex);
		auto elementFieldType = elementField->getTypeLayout();
		auto count = elementFieldType->isArray() ? elementFieldType->getFieldCount() : 1;
		uniformsTotalSize += count * createLayoutBindings<Vk>(
			elementField,
			genericParameterIndices,
			layouts,
			&bindingSpace,
			fullName.c_str());
	}

	for (auto categoryIndex = 0; categoryIndex < categoryCount; categoryIndex++)
	{
		auto subCategory = parameter->getCategoryByIndex(categoryIndex);
		
		if (subCategory == slang::ParameterCategory::DescriptorTableSlot || subCategory == slang::ParameterCategory::PushConstantBuffer)
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

}

template <>
ShaderModule<Vk>::ShaderModule(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    ShaderModuleHandle<Vk>&& shaderModule,
	const EntryPoint<Vk>& entryPoint)
: DeviceResource<Vk>(
    deviceContext,
    { std::get<0>(entryPoint).c_str() },
    1,
    VK_OBJECT_TYPE_SHADER_MODULE,
    reinterpret_cast<uint64_t*>(&shaderModule))
, myShaderModule(std::move(shaderModule))
, myEntryPoint(entryPoint)
{
}

template <>
ShaderModule<Vk>::ShaderModule(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    const Shader<Vk>& shader)
: ShaderModule<Vk>(
    deviceContext,
	createShaderModule(
		deviceContext->getDevice(),
		shader.first.size(),
		reinterpret_cast<const uint32_t *>(shader.first.data())),
	shader.second)
{
}

template <>
ShaderModule<Vk>::ShaderModule(ShaderModule&& other) noexcept
: DeviceResource<Vk>(std::move(other))
, myShaderModule(std::exchange(other.myShaderModule, {}))
, myEntryPoint(std::exchange(other.myEntryPoint, {}))
{
}

template <>
ShaderModule<Vk>::~ShaderModule()
{
    if (myShaderModule)
        vkDestroyShaderModule(getDeviceContext()->getDevice(), myShaderModule, nullptr);
}

template <>
ShaderModule<Vk>& ShaderModule<Vk>::operator=(ShaderModule&& other) noexcept
{
	DeviceResource<Vk>::operator=(std::move(other));
	myShaderModule = std::exchange(other.myShaderModule, {});
	myEntryPoint = std::exchange(other.myEntryPoint, {});
	return *this;
}

template <>
void ShaderModule<Vk>::swap(ShaderModule& rhs) noexcept
{
	DeviceResource<Vk>::swap(rhs);
	std::swap(myShaderModule, rhs.myShaderModule);
	std::swap(myEntryPoint, rhs.myEntryPoint);
}
