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
	switch (typeLayout->getKind())
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
	case slang::TypeReflection::Kind::None:
	case slang::TypeReflection::Kind::Struct:
	case slang::TypeReflection::Kind::Array:
	case slang::TypeReflection::Kind::Matrix:
	case slang::TypeReflection::Kind::Vector:
	case slang::TypeReflection::Kind::Scalar:
	case slang::TypeReflection::Kind::TextureBuffer:
	case slang::TypeReflection::Kind::ShaderStorageBuffer:
	case slang::TypeReflection::Kind::ParameterBlock:
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
void createLayoutBindings<Vk>(
    slang::VariableLayoutReflection* parameter,
	const std::vector<uint32_t>& genericParameterIndices,
    DescriptorSetLayoutBindingsMap<Vk>& bindings,
	const unsigned* parentSpace,
	const char* parentName,
	bool parentPushConstant)
{
	using MapType = std::remove_reference_t<decltype(bindings)>;
	using MapValueType = typename MapType::mapped_type;
	using BindingVectorType = std::tuple_element_t<0, MapValueType>;
	using BindingType = typename BindingVectorType::value_type;

	auto* typeLayout = parameter->getTypeLayout();
	auto type = typeLayout->getType();
	auto userAttributeCount = type->getUserAttributeCount();
	auto arrayElementCount = typeLayout->isArray() ? typeLayout->getElementCount() : 1;
	auto fieldCount = typeLayout->getFieldCount();
	auto* elementTypeLayout = typeLayout->getElementTypeLayout();
	auto elementKind = elementTypeLayout->getKind();
	auto elementFieldCount = elementTypeLayout->getFieldCount();
	auto genericParamIndex = elementTypeLayout->getGenericParamIndex();
	auto space = parameter->getBindingSpace();
	auto index = parameter->getBindingIndex();
	auto name = parameter->getName();
	auto stage = parameter->getStage();
	bool pushConstant = (parameter->findModifier(slang::Modifier::ID::PushConstant) ? true : false);

	std::string fullName;
	if (parentName)
	{
		fullName.append(parentName);
		fullName.append(".");
	}
	fullName.append(name);
	
	for (auto fieldIndex = 0; fieldIndex < fieldCount; fieldIndex++)
		createLayoutBindings<Vk>(
			typeLayout->getFieldByIndex(fieldIndex),
			genericParameterIndices,
			bindings,
			&space,
			name);

	auto categoryCount = parameter->getCategoryCount();
	for (auto categoryIndex = 0; categoryIndex < categoryCount; categoryIndex++)
	{
		auto category = parameter->getCategoryByIndex(categoryIndex);
		auto offsetForCategory = parameter->getOffset(category);
		auto spaceForCategory = parameter->getBindingSpace(category);
		auto elementStride = typeLayout->getElementStride(category);
		auto elementSize = elementTypeLayout->getSize(category);
		// auto sizeInBytes = typeLayout->getSize(slang::ParameterCategory::Uniform);
		// auto tRegCount = typeLayout->getSize(slang::ParameterCategory::ShaderResource);
		
		if (category == slang::ParameterCategory::DescriptorTableSlot)
		{
			std::cout << "name: " << name
				<< ", fullName: " << fullName
				<< ", space: " << space
				<< ", parent space: " << (parentSpace ? std::to_string(*parentSpace) : "(nullptr)")
				<< ", index: " << index
				<< ", stage: " << stage
				<< ", kind: " << (int)typeLayout->getKind()
				<< ", typeName: " << typeLayout->getName()
				<< ", userAttributeCount: " << userAttributeCount;
			
			std::cout << ", arrayElementCount: " << (typeLayout->isArray() ? arrayElementCount : 0)
				<< ", fieldCount: " << fieldCount;

			std::cout << ", category: " << category
				<< ", spaceForCategory: " << spaceForCategory
				<< ", offsetForCategory: " << offsetForCategory
				<< ", elementStride: " << elementStride
				<< ", elementSize: " << elementSize
				<< ", elementKind: " << (int)elementKind
				<< ", elementFieldCount: " << (int)elementKind
				<< ", genericParamIndex: " << genericParamIndex
				<< ", pushConstant: " << pushConstant
				<< ", parent pushConstant: " << parentPushConstant;

			std::cout << std::endl;

			BindingType binding;
			binding.binding = index;
			binding.descriptorType = shader::getDescriptorType<Vk>(typeLayout);
			binding.descriptorCount = arrayElementCount;
			binding.stageFlags = shader::getStageFlags<Vk>(stage);
			binding.pImmutableSamplers = nullptr;

			auto bindingSpace = (parentSpace ? *parentSpace : space);
            std::get<0>(bindings[bindingSpace]).push_back(binding);
			// todo: immutable samplers
			//std::get<1>(bindings[space]).push_back(SamplerCreateInfo<Vk>{});
			bool usePushConstant = parentPushConstant || pushConstant;
			std::get<2>(bindings[bindingSpace]) = (usePushConstant ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0);
		}
		else if (category == slang::ParameterCategory::RegisterSpace)
		{
			for (auto fieldIndex = 0; fieldIndex < elementFieldCount; fieldIndex++)
				createLayoutBindings<Vk>(
					elementTypeLayout->getFieldByIndex(fieldIndex),
					genericParameterIndices,
					bindings,
					(parentSpace ? parentSpace : &space),
					fullName.c_str(),
					parentPushConstant || pushConstant);
		}
		else
		{
			assert(false);
		}
	}
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
