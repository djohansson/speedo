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
	uint32_t currentSet,
	uint32_t& setCount,
	const char* parentName)
{
	using MapType = std::remove_reference_t<decltype(bindings)>;
	using MapValueType = typename MapType::mapped_type;
	using BindingVectorType = std::tuple_element_t<0, MapValueType>;
	using BindingType = typename BindingVectorType::value_type;

	auto* typeLayout = parameter->getTypeLayout();
	auto arrayElementCount = typeLayout->getElementCount();
	auto fieldCount = typeLayout->getFieldCount();
	auto* elementTypeLayout = typeLayout->getElementTypeLayout();
	auto elementKind = elementTypeLayout->getKind();
	auto elementFieldCount = elementTypeLayout->getFieldCount();
	auto genericParamIndex = elementTypeLayout->getGenericParamIndex();
	
	for (auto ff = 0; ff < fieldCount; ff++)
		createLayoutBindings<Vk>(
			typeLayout->getFieldByIndex(ff),
			genericParameterIndices,
			bindings,
			currentSet,
			setCount,
			parameter->getName());

	auto categoryCount = parameter->getCategoryCount();
	for (auto cc = 0; cc < categoryCount; cc++)
	{
		auto category = parameter->getCategoryByIndex(cc);
		auto offsetForCategory = parameter->getOffset(category);
		auto spaceForCategory = parameter->getBindingSpace(category);
		auto elementStride = typeLayout->getElementStride(category);
		auto elementSize = elementTypeLayout->getSize(category);
		
		if (category == slang::ParameterCategory::DescriptorTableSlot)
		{
			std::string name;
			if (parentName)
			{
				name.append(parentName);
				name.append(".");
			}
			name.append(parameter->getName());

			std::cout << "name: " << name
				<< ", set: " << currentSet
				<< ", index: " << parameter->getBindingIndex()
				<< ", space: " << parameter->getBindingSpace()
				<< ", stage: " << parameter->getStage()
				<< ", kind: " << (int)typeLayout->getKind()
				<< ", typeName: " << typeLayout->getName();
			
			std::cout << ", arrayElementCount: " << arrayElementCount
				<< ", fieldCount: " << fieldCount;

			std::cout << ", category: " << category
				<< ", offsetForCategory: " << offsetForCategory
				<< ", spaceForCategory: " << spaceForCategory
				<< ", elementStride: " << elementStride
				<< ", elementSize: " << elementSize
				<< ", elementKind: " << (int)elementKind
				<< ", elementFieldCount: " << (int)elementKind
				<< ", genericParamIndex: " << genericParamIndex;

			std::cout << std::endl;

			BindingType binding;
			binding.binding = parameter->getBindingIndex();
			binding.descriptorType = shader::getDescriptorType<Vk>(typeLayout);
			binding.descriptorCount = typeLayout->isArray() ? typeLayout->getElementCount() : 1;
			binding.stageFlags = shader::getStageFlags<Vk>(parameter->getStage());
			binding.pImmutableSamplers = nullptr; // todo - initialize these
            //binding.immutableSamplerCreateInfos.push_back(...)
			std::get<0>(bindings[currentSet]).push_back(binding);
			std::get<1>(bindings[currentSet]) = 0; //currentSet > 0 ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0;
		}
		else if (category == slang::ParameterCategory::RegisterSpace)
		{
			currentSet = ++setCount;
			for (auto ff = 0; ff < elementFieldCount; ff++)
				createLayoutBindings<Vk>(
					elementTypeLayout->getFieldByIndex(ff),
					genericParameterIndices,
					bindings,
					currentSet,
					setCount,
					parameter->getName());
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
