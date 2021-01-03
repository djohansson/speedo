#include "shader.h"
#include "vk-utils.h"

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
DescriptorType<Vk> getDescriptorType<Vk>(slang::TypeLayoutReflection* typeLayout)
{
	auto kind = typeLayout->getKind();
	switch (kind)
	{
	case slang::TypeReflection::Kind::ConstantBuffer:
		return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	case slang::TypeReflection::Kind::Resource:
	{
		auto type = typeLayout->getType();
		auto shape = type->getResourceShape();

		switch (shape & SLANG_RESOURCE_BASE_SHAPE_MASK)
		{
			case SLANG_TEXTURE_2D:
			case SLANG_TEXTURE_1D:
			case SLANG_TEXTURE_3D:
			case SLANG_TEXTURE_CUBE:
				return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
			//case SLANG_TEXTURE_BUFFER:
				//return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER?
			default:
				assert(false); // please implement me!
				break;
		}
		return DescriptorType<Vk>{};
	}
	case slang::TypeReflection::Kind::SamplerState:
		return VK_DESCRIPTOR_TYPE_SAMPLER;
	case slang::TypeReflection::Kind::ParameterBlock:
		return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	case slang::TypeReflection::Kind::None:
	case slang::TypeReflection::Kind::Struct:
	case slang::TypeReflection::Kind::Array:
	case slang::TypeReflection::Kind::Matrix:
	case slang::TypeReflection::Kind::Vector:
	case slang::TypeReflection::Kind::Scalar:
	case slang::TypeReflection::Kind::TextureBuffer:
	case slang::TypeReflection::Kind::ShaderStorageBuffer:
	case slang::TypeReflection::Kind::GenericTypeParameter:
	case slang::TypeReflection::Kind::Interface:
	case slang::TypeReflection::Kind::OutputStream:
	case slang::TypeReflection::Kind::Specialized:
	default:
		assert(false); // please implement me!
		break;
	}

	return DescriptorType<Vk>{};
}

template <>
uint32_t createLayoutBindings<Vk>(
    slang::VariableLayoutReflection* parameter,
	const std::vector<uint32_t>& genericParameterIndices,
    std::map<uint32_t, DescriptorSetLayoutCreateDesc<Vk>>& layouts,
	const unsigned* parentSpace,
	const char* parentName,
	bool parentPushConstant)
{
	using BindingType = DescriptorSetLayoutBinding<Vk>;
	
	auto space = parameter->getBindingSpace();
	auto index = parameter->getBindingIndex();
	auto name = std::string(parameter->getName());
	auto stage = parameter->getStage();
	bool pushConstant = (parameter->findModifier(slang::Modifier::ID::PushConstant) ? true : false);
	auto categoryCount = parameter->getCategoryCount();
	auto* typeLayout = parameter->getTypeLayout();
	auto type = typeLayout->getType();
	auto userAttributeCount = type->getUserAttributeCount();
	auto kind = typeLayout->getKind();
	auto typeName = typeLayout->getName();
	auto arrayElementCount = typeLayout->isArray() ? typeLayout->getElementCount() : 0;
	auto fieldCount = typeLayout->getFieldCount();
	auto* elementTypeLayout = typeLayout->getElementTypeLayout();
	auto elementKind = elementTypeLayout->getKind();
	auto elementFieldCount = elementTypeLayout->getFieldCount();
	auto genericParamIndex = elementTypeLayout->getGenericParamIndex();

	// auto* elementVarLayout = typeLayout->getElementVarLayout();

	// if (auto elementVarName = elementVarLayout->getName())
	// 	std::cout << "elementVarLayout name: " << elementVarName << std::endl;

	// std::cout << "elementVarLayout space: " << elementVarLayout->getBindingSpace() << std::endl;


	std::string fullName;
	if (parentName)
	{
		fullName.append(parentName);
		fullName.append(".");
	}
	fullName.append(name);

	auto bindingSpace = (parentSpace ? *parentSpace : space);
	bool usePushConstant = parentPushConstant || pushConstant;

	uint32_t fieldSize = 0;
	for (auto fieldIndex = 0; fieldIndex < fieldCount; fieldIndex++)
		fieldSize += createLayoutBindings<Vk>(
			typeLayout->getFieldByIndex(fieldIndex),
			genericParameterIndices,
			layouts,
			&bindingSpace,
			fullName.c_str(),
			usePushConstant);

	uint32_t elementFieldSize = 0;
	for (auto elementFieldIndex = 0; elementFieldIndex < elementFieldCount; elementFieldIndex++)
		elementFieldSize += createLayoutBindings<Vk>(
			elementTypeLayout->getFieldByIndex(elementFieldIndex),
			genericParameterIndices,
			layouts,
			&bindingSpace,
			fullName.c_str(),
			usePushConstant);

	uint32_t categoriesTotalSize = 0;
	for (auto categoryIndex = 0; categoryIndex < categoryCount; categoryIndex++)
	{
		auto category = parameter->getCategoryByIndex(categoryIndex);
		auto offsetForCategory = parameter->getOffset(category);
		auto spaceForCategory = parameter->getBindingSpace(category);
		auto elementStride = elementTypeLayout->getElementStride(category);
		auto elementSize = elementTypeLayout->getSize(category);
		auto sizeInBytes = typeLayout->getSize(category);
		categoriesTotalSize += sizeInBytes;

		std::cout << "DEBUG: name: " << name
			<< ", fullName: " << fullName
			<< ", space: " << space
			<< ", parent space: " << (parentSpace ? std::to_string(*parentSpace) : "(nullptr)")
			<< ", index: " << index
			<< ", stage: " << stage
			<< ", kind: " << (int)kind
			<< ", typeName: " << typeName
			<< ", userAttributeCount: " << userAttributeCount;
		
		std::cout << ", arrayElementCount: " << arrayElementCount
			<< ", fieldCount: " << fieldCount;

		std::cout << ", category: " << category
			<< ", spaceForCategory: " << spaceForCategory
			<< ", offsetForCategory: " << offsetForCategory
			<< ", elementStride: " << elementStride
			<< ", elementSize: " << elementSize
			<< ", elementKind: " << (int)elementKind
			<< ", elementFieldCount: " << elementFieldCount
			<< ", sizeInBytes: " << sizeInBytes
			<< ", genericParamIndex: " << genericParamIndex
			<< ", pushConstant: " << pushConstant
			<< ", parent pushConstant: " << parentPushConstant;

		std::cout << std::endl;

		auto addBinding = [&layouts](
			auto bindingSpace,
			auto typeLayout,
			auto usePushConstant,
			auto usePushDescriptor,
			auto arrayElementCount,
			auto elementFieldSize,
			auto stage,
			auto name)
		{
			auto getDescriptorCount = [](auto arrayElementCount, auto descriptorType)
			{
				size_t result = 1;

				if (arrayElementCount)
					result = arrayElementCount;

				if (descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT)
					result = roundUp(result, 4);

				return result;
			};

			auto& layout = layouts[bindingSpace];

			BindingType slot;
			slot.binding = layout.bindings.size();
			slot.descriptorType = shader::getDescriptorType<Vk>(typeLayout);
			slot.descriptorCount = getDescriptorCount(arrayElementCount, slot.descriptorType);
			slot.stageFlags = shader::getStageFlags<Vk>(stage);
			slot.pImmutableSamplers = nullptr;

			layout.bindings.push_back(slot);
			layout.variableNames.push_back(name);
			// todo: immutable samplers
			//layout.immutableSamplers.push_back(SamplerCreateInfo<Vk>{});
			if (usePushDescriptor)
				layout.flags |= VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;

			if (usePushConstant)
			{
				assert(!layout.pushConstantRange.has_value());
				assert(elementFieldSize > 0);
				layout.pushConstantRange = PushConstantRange<Vk>{slot.stageFlags, 0, roundUp(elementFieldSize, 4)};
			}

			std::cout << "Set: " << bindingSpace << ", Binding: " << slot.binding << " : " << name << std::endl;
		};

		if (category == slang::ParameterCategory::DescriptorTableSlot && kind != slang::TypeReflection::Kind::ParameterBlock)
		{
			addBinding(
				bindingSpace,
				typeLayout,
				usePushConstant && elementFieldSize > 0,
				usePushConstant, // yes
				arrayElementCount,
				elementFieldSize,
				stage,
				fullName);
		}
		// else if (category == slang::ParameterCategory::RegisterSpace)
		// {
		// 	addBinding(
		// 		bindingSpace,
		// 		typeLayout,
		// 		elementFieldSize > 0,
		// 		usePushConstant && usePushConstant, // yes
		// 		arrayElementCount,
		// 		elementFieldSize,
		// 		stage,
		// 		fullName);
		// }
	}

	return categoriesTotalSize;
}

}

template <>
ShaderModule<Vk>::ShaderModule(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    ShaderModuleHandle<Vk>&& shaderModule,
	const EntryPoint<Vk>& entryPoint)
: DeviceResource<Vk>(
    deviceContext,
    { entryPoint.first.c_str() },
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
ShaderModule<Vk>::ShaderModule(ShaderModule<Vk>&& other)
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
ShaderModule<Vk>& ShaderModule<Vk>::operator=(ShaderModule<Vk>&& other)
{
	DeviceResource<Vk>::operator=(std::move(other));
	myShaderModule = std::exchange(other.myShaderModule, {});
	myEntryPoint = std::exchange(other.myEntryPoint, {});
	return *this;
}
