#include "shader.h"

template <>
void createLayoutBindings<GraphicsBackend::Vulkan>(
    slang::VariableLayoutReflection* parameter,
    DescriptorSetLayoutBindingsMap<GraphicsBackend::Vulkan>& bindings)
{
	slang::TypeLayoutReflection* typeLayout = parameter->getTypeLayout();

	std::cout << "name: " << parameter->getName()
				<< ", index: " << parameter->getBindingIndex()
				<< ", space: " << parameter->getBindingSpace()
				<< ", stage: " << parameter->getStage()
				<< ", kind: " << (int)typeLayout->getKind()
				<< ", typeName: " << typeLayout->getName();

	unsigned categoryCount = parameter->getCategoryCount();
	for (unsigned cc = 0; cc < categoryCount; cc++)
	{
		slang::ParameterCategory category = parameter->getCategoryByIndex(cc);

		size_t offsetForCategory = parameter->getOffset(category);
		size_t spaceForCategory = parameter->getBindingSpace(category);

		std::cout << ", category: " << category
					<< ", offsetForCategory: " << offsetForCategory
					<< ", spaceForCategory: " << spaceForCategory;

		if (category == slang::ParameterCategory::DescriptorTableSlot)
		{
			using MapType = std::remove_reference_t<decltype(bindings)>;
			using VectorType = typename MapType::mapped_type;
			using BindingType = typename VectorType::value_type;

			BindingType binding;
			binding.binding = parameter->getBindingIndex();
			binding.descriptorCount = typeLayout->isArray() ? typeLayout->getElementCount() : 1;
			binding.stageFlags = VK_SHADER_STAGE_ALL;
			binding.pImmutableSamplers = nullptr; // todo - initialize these
            //binding.immutableSamplerCreateInfos.push_back(...)

			switch (parameter->getStage())
			{
			case SLANG_STAGE_VERTEX:
				binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
				break;
			case SLANG_STAGE_FRAGMENT:
				binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
				break;
			case SLANG_STAGE_HULL:
				binding.stageFlags = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
				break;
			case SLANG_STAGE_DOMAIN:
				binding.stageFlags = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
				break;
			case SLANG_STAGE_GEOMETRY:
				binding.stageFlags = VK_SHADER_STAGE_GEOMETRY_BIT;
				break;
			case SLANG_STAGE_COMPUTE:
				binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
				break;
			case SLANG_STAGE_RAY_GENERATION:
				binding.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
				break;
			case SLANG_STAGE_INTERSECTION:
				binding.stageFlags = VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
				break;
			case SLANG_STAGE_ANY_HIT:
				binding.stageFlags = VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
				break;
			case SLANG_STAGE_CLOSEST_HIT:
				binding.stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
				break;
			case SLANG_STAGE_MISS:
				binding.stageFlags = VK_SHADER_STAGE_MISS_BIT_KHR;
				break;
			case SLANG_STAGE_CALLABLE:
				binding.stageFlags = VK_SHADER_STAGE_CALLABLE_BIT_KHR;
				break;
			case SLANG_STAGE_NONE:
				break;
			default:
				assert(false); // please implement me!
				break;
			}

			switch (typeLayout->getKind())
			{
			case slang::TypeReflection::Kind::ConstantBuffer:
				binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
				break;
			case slang::TypeReflection::Kind::Resource:
				binding.descriptorType =
					VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; // "resource" might be more
														// generic tho...
				break;
			case slang::TypeReflection::Kind::SamplerState:
				binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
				break;
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

			bindings[parameter->getBindingSpace()].push_back(binding);
		}
	}

	std::cout << std::endl;

	// unsigned fieldCount = typeLayout->getFieldCount();
	// for (unsigned ff = 0; ff < fieldCount; ff++)
	// 	createLayoutBindings<GraphicsBackend::Vulkan>(typeLayout->getFieldByIndex(ff), bindings);
}