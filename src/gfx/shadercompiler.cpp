// the shader compiler library (see gfx/shadercompiler.h): slang and all the code that calls it, which the client loads
// only when it must compile a shader (gfx::ShaderLoader). it uses core's headers (assertions, containers) but none of its
// compiled code, which would be a second copy of core's globals beside the client's
#include <gfx/shadercompiler.h>

#include <core/assert.h>
#include <core/utils.h>

#ifdef None // None is defined in X11/X.h
#undef None
#endif
#ifdef Bool // Bool is defined in X11/X.h
#undef Bool
#endif
#include <slang.h>

#include <xxhash.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#	define SPEEDO_SHADERCOMPILER_EXPORT extern "C" __declspec(dllexport)
#else
#	define SPEEDO_SHADERCOMPILER_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace gfx::shadercompiler
{

namespace
{

// the slang target and profile that produce shaders of format
std::tuple<SlangCompileTarget, const char*> SlangTarget(rhi::ShaderFormat format)
{
	switch (format)
	{
	case rhi::ShaderFormat::kSpirv16: return {SLANG_SPIRV, "SPIRV_1_6"};
	case rhi::ShaderFormat::kMsl: return {SLANG_METAL, ""};
	// 6.8 for SV_StartInstanceLocation (the indirect draws name their records by their first instance, which D3D12's
	// SV_InstanceID doesn't count; 6.6 refused it); DXC compiles and validates the DXIL
	case rhi::ShaderFormat::kDxil: return {SLANG_DXIL, "sm_6_8"};
	}
	ASSERT(false);
	return {SLANG_TARGET_UNKNOWN, ""};
}

rhi::ShaderStage GetStage(SlangStage stage)
{
	switch (stage)
	{
	case SLANG_STAGE_VERTEX:
		return rhi::ShaderStage::kVertex;
	case SLANG_STAGE_FRAGMENT:
		return rhi::ShaderStage::kFragment;
	case SLANG_STAGE_HULL:
		return rhi::ShaderStage::kTessellationControl;
	case SLANG_STAGE_DOMAIN:
		return rhi::ShaderStage::kTessellationEvaluation;
	case SLANG_STAGE_GEOMETRY:
		return rhi::ShaderStage::kGeometry;
	case SLANG_STAGE_COMPUTE:
		return rhi::ShaderStage::kCompute;
	case SLANG_STAGE_RAY_GENERATION:
		return rhi::ShaderStage::kRayGeneration;
	case SLANG_STAGE_INTERSECTION:
		return rhi::ShaderStage::kIntersection;
	case SLANG_STAGE_ANY_HIT:
		return rhi::ShaderStage::kAnyHit;
	case SLANG_STAGE_CLOSEST_HIT:
		return rhi::ShaderStage::kClosestHit;
	case SLANG_STAGE_MISS:
		return rhi::ShaderStage::kMiss;
	case SLANG_STAGE_CALLABLE:
		return rhi::ShaderStage::kCallable;
	case SLANG_STAGE_NONE:
		return rhi::ShaderStage::kAll; // todo: fix when slang handles this case.
	default:
		ASSERT(false); // please implement me!
	}

	return rhi::ShaderStage::kNone;
}

rhi::DescriptorType GetDescriptorType(slang::TypeReflection::Kind kind, SlangResourceShape shape, SlangResourceAccess access)
{
	auto type = rhi::DescriptorType{};

	switch (kind)
	{
	case slang::TypeReflection::Kind::ConstantBuffer:
		type = rhi::DescriptorType::kUniformBuffer;
		break;
	case slang::TypeReflection::Kind::TextureBuffer:
		type = rhi::DescriptorType::kUniformTexelBuffer; //?
		break;
	case slang::TypeReflection::Kind::ShaderStorageBuffer:
		type = rhi::DescriptorType::kStorageBuffer;
		break;
	case slang::TypeReflection::Kind::Resource: {
		switch (shape & SLANG_RESOURCE_BASE_SHAPE_MASK)
		{
		case SLANG_TEXTURE_1D:
		case SLANG_TEXTURE_2D:
		case SLANG_TEXTURE_3D:
		case SLANG_TEXTURE_CUBE:
			type =
				(access == SLANG_RESOURCE_ACCESS_READ_WRITE ? rhi::DescriptorType::kStorageImage
															: rhi::DescriptorType::kSampledImage);
			break;
		case SLANG_STRUCTURED_BUFFER:
			type = rhi::DescriptorType::kStorageBuffer;
			break;
		case SLANG_ACCELERATION_STRUCTURE:
			type = rhi::DescriptorType::kAccelerationStructure;
			break;
		default:
			ASSERT(false); // please implement me!
			break;
		}
		break;
	}
	case slang::TypeReflection::Kind::SamplerState:
		type = rhi::DescriptorType::kSampler;
		break;
	case slang::TypeReflection::Kind::ParameterBlock:
		type = rhi::DescriptorType::kInlineUniformBlock;
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
		ASSERT(false); // please implement me!
		break;
	}

	return type;
}

void AddBinding(
	unsigned bindingIndex,
	unsigned bindingSpace,
	slang::TypeLayoutReflection* typeLayout,
	bool usePushConstant,
	size_t sizeBytes,
	SlangStage stage,
	std::string_view name,
	core::UnorderedMap<uint32_t, rhi::ShaderSetLayout>& layouts)
{
	ENSURE(typeLayout != nullptr);

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

	auto descriptorType = GetDescriptorType(kind, shape, access);

	auto isInlineUniformBlock = descriptorType == rhi::DescriptorType::kInlineUniformBlock;

	auto& binding = layout.bindings.emplace_back(rhi::ShaderBinding{
		.binding = bindingIndex,
		.type = descriptorType,
		.count = isInlineUniformBlock ? static_cast<uint32_t>(sizeBytes) : static_cast<uint32_t>(descriptorCount),
		.stages = GetStage(stage),
		.name = std::string(name),
		.nameHash = XXH3_64bits(name.data(), name.size())});

	if (usePushConstant)
	{
		ASSERT(!layout.pushConstants);
		ASSERT(sizeBytes > 0);
		ASSERT(!isInlineUniformBlock);
		layout.pushConstants =
			rhi::ShaderPushConstants{.stages = binding.stages, .offset = 0, .size = static_cast<uint32_t>(sizeBytes)};
	}

	if (Logging())
		std::println(
			"ADD BINDING \"{}\": Set: {}, Binding: {}, Count: {}, Size: {}",
			name,
			bindingSpace,
			binding.binding,
			descriptorCount,
			sizeBytes);
}

uint32_t CreateLayoutBindings(
	slang::VariableLayoutReflection* parameter,
	const std::vector<uint32_t>& genericParameterIndices,
	core::UnorderedMap<uint32_t, rhi::ShaderSetLayout>& layouts,
	const unsigned* parentSpace = nullptr,
	const char* parentName = nullptr)
{
	ENSURE(parameter != nullptr);
	auto space = parameter->getBindingSpace();
	auto index = parameter->getBindingIndex();
	auto name = std::string(parameter->getName());
	auto stage = parameter->getStage();
	auto categoryCount = parameter->getCategoryCount();
	auto* typeLayout = parameter->getTypeLayout();
	ENSURE(typeLayout != nullptr);
	auto arrayElementCount = typeLayout->getElementCount();
	auto fieldCount = typeLayout->getFieldCount();
	auto* elementTypeLayout = typeLayout->getElementTypeLayout();
	auto elementFieldCount = (elementTypeLayout != nullptr) ? elementTypeLayout->getFieldCount() : 0;

	auto category = parameter->getCategory();
	const auto* typeName = typeLayout->getName();
	ENSURE(typeName != nullptr);
	auto kind = typeLayout->getKind();
	auto* type = typeLayout->getType();
	ENSURE(type != nullptr);
	auto userAttributeCount = type->getUserAttributeCount();
	auto elementKind = (elementTypeLayout != nullptr) ? elementTypeLayout->getKind() : slang::TypeReflection::Kind::None;
	auto genericParamIndex = (elementTypeLayout != nullptr) ? elementTypeLayout->getGenericParamIndex() : 0;

	std::string fullName;
	if (parentName != nullptr)
	{
		fullName.append(parentName);
		fullName.append(".");
	}
	fullName.append(name);

	auto bindingSpace = ((parentSpace != nullptr) ? *parentSpace : space);

	uint32_t uniformsTotalSize = 0;

	for (auto categoryIndex = 0; std::cmp_less(categoryIndex, categoryCount); categoryIndex++)
	{
		auto subCategory = static_cast<SlangParameterCategory>(parameter->getCategoryByIndex(categoryIndex));
		auto spaceForCategory = parameter->getBindingSpace(subCategory);
		auto elementSize = (elementTypeLayout != nullptr) ? elementTypeLayout->getSize(subCategory) : 0;
		auto elementAlignment = (elementTypeLayout != nullptr) ? elementTypeLayout->getAlignment(subCategory) : 0;
		auto size = typeLayout->getSize(subCategory);
		auto alignment = typeLayout->getAlignment(subCategory);

		auto offsetForCategory = parameter->getOffset(subCategory);
		auto elementStride = (elementTypeLayout != nullptr) ? elementTypeLayout->getElementStride(subCategory) : 0;

		if (Logging())
			std::println(
				"DEBUG: name: {}, fullName: {}, space: {}, parent space: {}, index: {}, stage: {}, kind: {}, typeName: {}, "
				"userAttributeCount: {}, arrayElementCount: {}, fieldCount: {}, category: {}, subCategory: {}, "
				"spaceForCategory: {}, offsetForCategory: {}, elementStride: {}, elementSize: {}, elementAlignment: {}, "
				"elementKind: {}, elementFieldCount: {}, size: {}, alignment: {}, genericParamIndex: {}",
				name,
				fullName,
				space,
				(parentSpace != nullptr) ? std::to_string(*parentSpace) : "(nullptr)",
				index,
				static_cast<int>(stage),
				static_cast<int>(kind),
				(typeName != nullptr) ? typeName : "(nullptr)",
				userAttributeCount,
				arrayElementCount,
				fieldCount,
				static_cast<int>(category),
				static_cast<int>(subCategory),
				spaceForCategory,
				offsetForCategory,
				elementStride,
				elementSize,
				elementAlignment,
				static_cast<int>(elementKind),
				elementFieldCount,
				size,
				alignment,
				genericParamIndex);

		if (subCategory == SLANG_PARAMETER_CATEGORY_REGISTER_SPACE)
			bindingSpace = spaceForCategory;
		else if (subCategory == SLANG_PARAMETER_CATEGORY_SUB_ELEMENT_REGISTER_SPACE)
			bindingSpace = static_cast<unsigned>(offsetForCategory); // a parameter block's set: its fields' (see bindings.slang)
		else if (subCategory == SLANG_PARAMETER_CATEGORY_UNIFORM)
			uniformsTotalSize += size;
	}

	// a parameter block's fields are a descriptor set's bindings, named as the fields are (gfx binds them by name, see
	// bindings.slang); a struct's are named by their path
	const bool parameterBlock = kind == slang::TypeReflection::Kind::ParameterBlock;
	for (auto elementFieldIndex = 0; std::cmp_less(elementFieldIndex, elementFieldCount); elementFieldIndex++)
	{
		auto* elementField = (elementTypeLayout != nullptr) ? elementTypeLayout->getFieldByIndex(elementFieldIndex) : nullptr;
		auto* elementFieldType = (elementField != nullptr) ? elementField->getTypeLayout() : nullptr;
		auto count = (elementFieldType != nullptr) && elementFieldType->isArray() ? elementFieldType->getFieldCount() : 1;
		uniformsTotalSize += count * CreateLayoutBindings(
			elementField, genericParameterIndices, layouts, &bindingSpace, parameterBlock ? nullptr : fullName.c_str());
	}

	// a binding of the parameter: the push constants' once, where it has their category (a D3D register given them, b0 in
	// bindings.slang, has the reflection list a descriptor table slot of theirs too: two bindings of one number, which
	// Vulkan's layouts refused)
	bool pushConstants = false;
	for (auto categoryIndex = 0; std::cmp_less(categoryIndex, categoryCount); categoryIndex++)
		pushConstants = pushConstants || parameter->getCategoryByIndex(categoryIndex) == slang::ParameterCategory::PushConstantBuffer;
	for (auto categoryIndex = 0; std::cmp_less(categoryIndex, categoryCount); categoryIndex++)
	{
		auto subCategory = parameter->getCategoryByIndex(categoryIndex);

		if ((subCategory == slang::ParameterCategory::DescriptorTableSlot && !pushConstants) ||
			subCategory == slang::ParameterCategory::PushConstantBuffer)
		{
			AddBinding(
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

// a buffer's data laid out packed, as slang lays out every buffer of Metal's (its natural layout, which Metal's target
// always takes, whatever the buffer's annotation: each value at its scalar's alignment, a struct's size not rounded up to
// a vector's), against the reflection target's layout (spir-v's: std430, std140 for a constant buffer): the backends read
// the same bytes where the two agree, so a buffer's struct pads explicitly where std430 would (rhitestdraw.slang's draw
// record, 36 bytes packed and 48 std430, read its second record from the first's padding on Metal). mismatches named by
// their path are appended to error
struct PackedLayout
{
	size_t size = 0;
	size_t alignment = 1;
};

size_t PackedScalarSize(slang::TypeReflection::ScalarType type)
{
	switch (type)
	{
	case slang::TypeReflection::ScalarType::Int64:
	case slang::TypeReflection::ScalarType::UInt64:
	case slang::TypeReflection::ScalarType::Float64:
		return 8;
	case slang::TypeReflection::ScalarType::Float16:
	case slang::TypeReflection::ScalarType::Int16:
	case slang::TypeReflection::ScalarType::UInt16:
		return 2;
	case slang::TypeReflection::ScalarType::Int8:
	case slang::TypeReflection::ScalarType::UInt8:
		return 1;
	default:
		return 4;
	}
}

PackedLayout CheckPackedLayout(slang::TypeLayoutReflection* type, const std::string& path, std::vector<std::string>& mismatches)
{
	switch (type->getKind())
	{
	case slang::TypeReflection::Kind::Scalar:
	{
		const auto size = PackedScalarSize(type->getScalarType());
		return {.size = size, .alignment = size};
	}
	case slang::TypeReflection::Kind::Vector:
	case slang::TypeReflection::Kind::Matrix:
	{
		const auto scalar = PackedScalarSize(type->getScalarType());
		const size_t count = type->getKind() == slang::TypeReflection::Kind::Vector
								 ? type->getElementCount()
								 : size_t{type->getRowCount()} * type->getColumnCount();
		if (type->getKind() == slang::TypeReflection::Kind::Matrix && type->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) != scalar * count)
			mismatches.push_back(std::format(
				"{}: a matrix of {} bytes packed, {} reflected", path, scalar * count, type->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM)));
		return {.size = scalar * count, .alignment = scalar};
	}
	case slang::TypeReflection::Kind::Array:
	{
		auto element = CheckPackedLayout(type->getElementTypeLayout(), path + "[]", mismatches);
		const auto stride = type->getElementStride(SLANG_PARAMETER_CATEGORY_UNIFORM);
		if (element.size != stride)
			mismatches.push_back(std::format("{}: elements {} bytes apart packed, {} reflected", path, element.size, stride));
		return {.size = element.size * type->getElementCount(), .alignment = element.alignment};
	}
	case slang::TypeReflection::Kind::Struct:
	{
		size_t offset = 0;
		size_t alignment = 1;
		for (unsigned fieldIt = 0; fieldIt < type->getFieldCount(); fieldIt++)
		{
			auto* field = type->getFieldByIndex(fieldIt);
			const auto fieldPath = std::format("{}.{}", path, field->getName());
			auto layout = CheckPackedLayout(field->getTypeLayout(), fieldPath, mismatches);
			offset = (offset + layout.alignment - 1) / layout.alignment * layout.alignment;
			if (const auto reflected = field->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM); offset != reflected)
				mismatches.push_back(std::format("{}: at {} packed, {} reflected", fieldPath, offset, reflected));
			offset += layout.size;
			alignment = std::max(alignment, layout.alignment);
		}
		return {.size = (offset + alignment - 1) / alignment * alignment, .alignment = alignment};
	}
	default:
		return {};
	}
}

// each buffer's data (a structured buffer's element, a constant buffer's or the push constants' struct, in a parameter
// block's fields or global), checked packed against the reflection target's layout (see CheckPackedLayout)
void CheckPackedBuffers(slang::TypeLayoutReflection* type, const std::string& path, std::vector<std::string>& mismatches)
{
	switch (type->getKind())
	{
	case slang::TypeReflection::Kind::ParameterBlock:
	{
		auto* element = type->getElementTypeLayout();
		for (unsigned fieldIt = 0; element != nullptr && fieldIt < element->getFieldCount(); fieldIt++)
		{
			auto* field = element->getFieldByIndex(fieldIt);
			CheckPackedBuffers(field->getTypeLayout(), field->getName(), mismatches);
		}
		break;
	}
	case slang::TypeReflection::Kind::Array:
		CheckPackedBuffers(type->getElementTypeLayout(), path, mismatches);
		break;
	case slang::TypeReflection::Kind::ConstantBuffer:
		CheckPackedLayout(type->getElementTypeLayout(), path, mismatches);
		break;
	case slang::TypeReflection::Kind::Resource:
		if (auto* element = type->getElementTypeLayout(); element != nullptr && element->getKind() == slang::TypeReflection::Kind::Struct)
		{
			auto layout = CheckPackedLayout(element, path + "[]", mismatches);
			const auto stride = type->getElementStride(SLANG_PARAMETER_CATEGORY_UNIFORM);
			const auto reflected = stride != 0 ? stride : element->getStride(SLANG_PARAMETER_CATEGORY_UNIFORM);
			if (layout.size != reflected)
				mismatches.push_back(std::format("{}[]: elements {} bytes apart packed, {} reflected", path, layout.size, reflected));
		}
		break;
	default:
		break;
	}
}

// each set's and the push constants' slot in the code target's layout (MSL's buffer indices, see rhi::kNoShaderSlot): a
// parameter block's set and the push constants' as CreateLayoutBindings finds them in the reflection target's. an error
// for a resource outside a parameter block, which has no set of its own in such code
std::string AssignSlots(
	SlangCompileRequest* slangRequest,
	int codeTargetIndex,
	slang::ShaderReflection* reflection,
	core::UnorderedMap<uint32_t, rhi::ShaderSetLayout>& layouts)
{
	slang::IComponentType* program = nullptr;
	if (SLANG_FAILED(spCompileRequest_getProgram(slangRequest, &program)) || program == nullptr)
		return "failed to get the program's layout for its code target";
	slang::ShaderReflection* codeLayout = program->getLayout(codeTargetIndex);
	std::string error;
	if (codeLayout == nullptr || codeLayout->getParameterCount() != reflection->getParameterCount())
		error = "the code target's layout has other parameters than the reflection target's";
	for (unsigned parameterIt = 0; error.empty() && parameterIt < reflection->getParameterCount(); parameterIt++)
	{
		auto* parameter = reflection->getParameterByIndex(parameterIt);
		auto* codeParameter = codeLayout->getParameterByIndex(parameterIt);
		unsigned space = parameter->getBindingSpace();
		bool pushConstants = false;
		for (unsigned categoryIt = 0; categoryIt < parameter->getCategoryCount(); categoryIt++)
		{
			auto category = static_cast<SlangParameterCategory>(parameter->getCategoryByIndex(categoryIt));
			if (category == SLANG_PARAMETER_CATEGORY_REGISTER_SPACE)
				space = parameter->getBindingSpace(category);
			else if (category == SLANG_PARAMETER_CATEGORY_SUB_ELEMENT_REGISTER_SPACE)
				space = static_cast<unsigned>(parameter->getOffset(category));
			else if (category == SLANG_PARAMETER_CATEGORY_PUSH_CONSTANT_BUFFER)
				pushConstants = true;
		}
		const bool parameterBlock = parameter->getType()->getKind() == slang::TypeReflection::Kind::ParameterBlock;
		std::optional<uint32_t> slot;
		bool resource = false; // a buffer, texture or sampler the code binds (not a specialization constant: a function constant)
		for (unsigned categoryIt = 0; categoryIt < codeParameter->getCategoryCount(); categoryIt++)
		{
			auto category = static_cast<SlangParameterCategory>(codeParameter->getCategoryByIndex(categoryIt));
			if (category == SLANG_PARAMETER_CATEGORY_METAL_BUFFER)
				slot = static_cast<uint32_t>(codeParameter->getOffset(category));
			resource = resource || category == SLANG_PARAMETER_CATEGORY_METAL_BUFFER || category == SLANG_PARAMETER_CATEGORY_METAL_TEXTURE ||
					   category == SLANG_PARAMETER_CATEGORY_METAL_SAMPLER || category == SLANG_PARAMETER_CATEGORY_METAL_ARGUMENT_BUFFER_ELEMENT;
		}
		if (!parameterBlock && !pushConstants)
		{
			if (resource)
				error = std::format("{}: a resource outside a parameter block (its set has no slot in the code)", parameter->getName());
			continue;
		}
		if (!slot)
		{
			error = std::format("{}: no buffer index in the code target's layout", parameter->getName());
			continue;
		}
		auto layoutIt = layouts.find(space);
		if (layoutIt == layouts.end())
			continue; // a set without bindings
		auto& layout = layoutIt->second;
		if (pushConstants && layout.pushConstants)
			layout.pushConstants->slot = *slot;
		else if (!pushConstants)
			layout.slot = *slot;
	}
	program->release();
	return error;
}

// a buffer's data laid out by the code target as by the reflection target (DXIL's: a constant buffer's or the root
// constants' packing, which keeps a vector within 16 bytes and an array's elements 16 bytes apart, and a structured
// buffer's, which pads nothing: a float3 element is 12 bytes apart where std430 has 16), so that the backends read the
// same bytes; mismatches named by their path are appended
void CompareUniformLayout(
	slang::TypeLayoutReflection* reflected, slang::TypeLayoutReflection* code, const std::string& path, std::vector<std::string>& mismatches)
{
	if (reflected == nullptr || code == nullptr)
		return;
	const auto reflectedSize = reflected->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM);
	const auto codeSize = code->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM);
	switch (reflected->getKind())
	{
	case slang::TypeReflection::Kind::Struct:
		for (unsigned fieldIt = 0; fieldIt < reflected->getFieldCount() && fieldIt < code->getFieldCount(); fieldIt++)
		{
			auto* reflectedField = reflected->getFieldByIndex(fieldIt);
			auto* codeField = code->getFieldByIndex(fieldIt);
			const auto fieldPath = std::format("{}.{}", path, reflectedField->getName());
			if (reflectedField->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM) != codeField->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM))
				mismatches.push_back(std::format(
					"{}: at {} in the code, {} reflected",
					fieldPath,
					codeField->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM),
					reflectedField->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM)));
			CompareUniformLayout(reflectedField->getTypeLayout(), codeField->getTypeLayout(), fieldPath, mismatches);
		}
		break;
	case slang::TypeReflection::Kind::Array:
		if (reflected->getElementStride(SLANG_PARAMETER_CATEGORY_UNIFORM) != code->getElementStride(SLANG_PARAMETER_CATEGORY_UNIFORM))
			mismatches.push_back(std::format(
				"{}: elements {} bytes apart in the code, {} reflected",
				path,
				code->getElementStride(SLANG_PARAMETER_CATEGORY_UNIFORM),
				reflected->getElementStride(SLANG_PARAMETER_CATEGORY_UNIFORM)));
		CompareUniformLayout(reflected->getElementTypeLayout(), code->getElementTypeLayout(), path + "[]", mismatches);
		break;
	case slang::TypeReflection::Kind::Scalar:
	case slang::TypeReflection::Kind::Vector:
	case slang::TypeReflection::Kind::Matrix:
		if (reflectedSize != codeSize)
			mismatches.push_back(std::format("{}: {} bytes in the code, {} reflected", path, codeSize, reflectedSize));
		break;
	default:
		break;
	}
}

std::string CompileSpecializations(
	const Request& request, SlangCompileRequest* compiled, int codeTargetIndex, slang::ShaderReflection* codeLayout, rhi::ShaderSet& shaderSet);

// a parameter's register of a D3D register class, and the class, where it has one (none: a uniform, or a block)
std::optional<std::pair<SlangParameterCategory, uint32_t>> D3DRegister(slang::VariableLayoutReflection* variable, size_t containerOffset = 0)
{
	for (unsigned categoryIt = 0; categoryIt < variable->getCategoryCount(); categoryIt++)
	{
		auto category = static_cast<SlangParameterCategory>(variable->getCategoryByIndex(categoryIt));
		if (category == SLANG_PARAMETER_CATEGORY_SHADER_RESOURCE || category == SLANG_PARAMETER_CATEGORY_UNORDERED_ACCESS ||
			category == SLANG_PARAMETER_CATEGORY_CONSTANT_BUFFER || category == SLANG_PARAMETER_CATEGORY_SAMPLER_STATE)
			return std::make_pair(category, static_cast<uint32_t>(containerOffset + variable->getOffset(category)));
	}
	return std::nullopt;
}

// DXIL's registers (see rhi::kNoShaderSlot), from the code target's layout, whose parameters are the reflection target's in
// the same order: each parameter block's register space (its set's slot), each of its fields' register (its binding's
// slot, matched by name), a storage buffer's class (written where it's an unordered access view) and its element's size
// in the code's layout (a structured buffer view's stride), and the push constants' register and space; then the
// specialization constant's values compiled each (CompileSpecializations). every buffer's data laid out as the reflection
// target lays it out (CompareUniformLayout). an error for a resource outside a parameter block, which has no set of its own
std::string AssignRegisters(
	const Request& request, SlangCompileRequest* slangRequest, int codeTargetIndex, slang::ShaderReflection* reflection, rhi::ShaderSet& shaderSet)
{
	slang::IComponentType* program = nullptr;
	if (SLANG_FAILED(spCompileRequest_getProgram(slangRequest, &program)) || program == nullptr)
		return "failed to get the program's layout for its code target";
	slang::ShaderReflection* codeLayout = program->getLayout(codeTargetIndex);
	std::string error;
	std::vector<std::string> mismatches;
	if (codeLayout == nullptr || codeLayout->getParameterCount() != reflection->getParameterCount())
		error = "the code target's layout has other parameters than the reflection target's";
	auto& layouts = shaderSet.layouts;
	for (unsigned parameterIt = 0; error.empty() && parameterIt < reflection->getParameterCount(); parameterIt++)
	{
		auto* parameter = reflection->getParameterByIndex(parameterIt);
		auto* codeParameter = codeLayout->getParameterByIndex(parameterIt);
		unsigned set = parameter->getBindingSpace();
		bool pushConstants = false;
		for (unsigned categoryIt = 0; categoryIt < parameter->getCategoryCount(); categoryIt++)
		{
			auto category = static_cast<SlangParameterCategory>(parameter->getCategoryByIndex(categoryIt));
			if (category == SLANG_PARAMETER_CATEGORY_REGISTER_SPACE)
				set = parameter->getBindingSpace(category);
			else if (category == SLANG_PARAMETER_CATEGORY_SUB_ELEMENT_REGISTER_SPACE)
				set = static_cast<unsigned>(parameter->getOffset(category));
			else if (category == SLANG_PARAMETER_CATEGORY_PUSH_CONSTANT_BUFFER)
				pushConstants = true;
		}
		const bool parameterBlock = parameter->getType()->getKind() == slang::TypeReflection::Kind::ParameterBlock;
		auto layoutIt = layouts.find(set);
		if (pushConstants)
		{
			auto reg = D3DRegister(codeParameter);
			if (!reg || reg->first != SLANG_PARAMETER_CATEGORY_CONSTANT_BUFFER)
			{
				error = std::format("{}: the push constants aren't a constant buffer in the code target's layout", parameter->getName());
				continue;
			}
			if (layoutIt != layouts.end() && layoutIt->second.pushConstants)
			{
				layoutIt->second.pushConstants->slot = reg->second;
				layoutIt->second.pushConstants->space = static_cast<uint32_t>(codeParameter->getBindingSpace(SLANG_PARAMETER_CATEGORY_CONSTANT_BUFFER));
			}
			CompareUniformLayout(
				parameter->getTypeLayout()->getElementTypeLayout(),
				codeParameter->getTypeLayout()->getElementTypeLayout(),
				parameter->getName(),
				mismatches);
			continue;
		}
		if (!parameterBlock)
		{
			if (D3DRegister(codeParameter))
				error = std::format("{}: a resource outside a parameter block (its set has no space in the code)", parameter->getName());
			continue;
		}
		// the block's space, and its fields' registers in it (the element's offsets in the block, then each field's)
		std::optional<uint32_t> space;
		for (unsigned categoryIt = 0; categoryIt < codeParameter->getCategoryCount(); categoryIt++)
		{
			auto category = static_cast<SlangParameterCategory>(codeParameter->getCategoryByIndex(categoryIt));
			if (category == SLANG_PARAMETER_CATEGORY_REGISTER_SPACE || category == SLANG_PARAMETER_CATEGORY_SUB_ELEMENT_REGISTER_SPACE)
				space = static_cast<uint32_t>(codeParameter->getOffset(category));
		}
		if (!space)
		{
			error = std::format("{}: no register space in the code target's layout", parameter->getName());
			continue;
		}
		if (layoutIt == layouts.end())
			continue; // a set without bindings
		auto& layout = layoutIt->second;
		layout.slot = *space;
		auto* reflectedElement = parameter->getTypeLayout()->getElementTypeLayout();
		auto* codeElementVariable = codeParameter->getTypeLayout()->getElementVarLayout();
		auto* codeElement = codeElementVariable->getTypeLayout();
		if (codeElement->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) > 0)
		{
			error = std::format("{}: a parameter block with uniforms (an implicit constant buffer), which the D3D12 backend doesn't bind", parameter->getName());
			continue;
		}
		for (unsigned fieldIt = 0; fieldIt < codeElement->getFieldCount(); fieldIt++)
		{
			auto* field = codeElement->getFieldByIndex(fieldIt);
			std::string_view name(field->getName());
			auto bindingIt = std::ranges::find(layout.bindings, name, &rhi::ShaderBinding::name);
			if (bindingIt == layout.bindings.end())
				continue; // nothing the reflection binds (unused)
			std::optional<std::pair<SlangParameterCategory, uint32_t>> reg;
			for (unsigned categoryIt = 0; categoryIt < field->getCategoryCount() && !reg; categoryIt++)
			{
				auto category = static_cast<SlangParameterCategory>(field->getCategoryByIndex(categoryIt));
				reg = D3DRegister(field, codeElementVariable->getOffset(category));
			}
			if (!reg)
			{
				error = std::format("{}: no register in the code target's layout", name);
				break;
			}
			bindingIt->slot = reg->second;
			bindingIt->writable = reg->first == SLANG_PARAMETER_CATEGORY_UNORDERED_ACCESS;
			// a buffer's element (through an array of buffers): its stride, and its data laid out as reflected
			auto* codeType = field->getTypeLayout();
			auto* reflectedType = fieldIt < reflectedElement->getFieldCount() ? reflectedElement->getFieldByIndex(fieldIt)->getTypeLayout() : nullptr;
			if (codeType->getKind() == slang::TypeReflection::Kind::Array)
			{
				codeType = codeType->getElementTypeLayout();
				reflectedType = reflectedType != nullptr ? reflectedType->getElementTypeLayout() : nullptr;
			}
			auto* codeData = codeType->getElementTypeLayout();
			auto* reflectedData = reflectedType != nullptr ? reflectedType->getElementTypeLayout() : nullptr;
			if (bindingIt->type == rhi::DescriptorType::kStorageBuffer && codeData != nullptr)
			{
				bindingIt->stride = static_cast<uint32_t>(codeData->getStride(SLANG_PARAMETER_CATEGORY_UNIFORM));
				if (reflectedData != nullptr && reflectedData->getStride(SLANG_PARAMETER_CATEGORY_UNIFORM) != bindingIt->stride)
					mismatches.push_back(std::format(
						"{}[]: elements {} bytes apart in the code, {} reflected",
						name,
						bindingIt->stride,
						reflectedData->getStride(SLANG_PARAMETER_CATEGORY_UNIFORM)));
			}
			if (bindingIt->type == rhi::DescriptorType::kStorageBuffer || bindingIt->type == rhi::DescriptorType::kUniformBuffer)
				CompareUniformLayout(reflectedData, codeData, std::string(name), mismatches);
		}
	}
	// the specialization constant's values compiled each (DXIL has no specialization constants: CompileSpecializations)
	if (error.empty())
		error = CompileSpecializations(request, slangRequest, codeTargetIndex, codeLayout, shaderSet);
	program->release();
	if (error.empty() && !mismatches.empty())
	{
		error = "buffers' data the DXIL target lays out otherwise than the reflection target (the backends would read other bytes: pad explicitly):";
		for (const auto& mismatch : mismatches)
			error += std::format("\n  {}", mismatch);
	}
	return error;
}

// one compile at a time (slang isn't thread safe, even across sessions: see gfx::ShaderLoader), and the session made at
// the first, kept until the process ends
std::mutex gMutex;
SlangSession* gSession = nullptr;

// the specialization constant 0 (slang's [vk::constant_id(0)], kSkippedLayers in bindings.slang) as a literal, for a
// format without specialization constants (DXIL): the shaders declare it a static const of this definition where it is
// defined, so that the compiler folds it as Vulkan's driver does a specialization
constexpr const char* kSpecializationDefinition = "SHADER_SPECIALIZATION_CONSTANT_0";

// a compile request set up as request asks: its search paths, options, definitions (and specialization, where given),
// and targets (the reflection's first, where asked: see below). the code target's index, or an error
std::pair<int, std::string> ConfigureRequest(
	SlangCompileRequest* slangRequest, const Request& request, std::optional<uint32_t> specialization, bool reflectionTarget)
{
	for (const auto& includePath : request.includePaths)
	{
		std::error_code error;
		auto path = std::filesystem::canonical(includePath, error);
		if (error || !std::filesystem::is_directory(path))
			return {-1, std::format("the include path {} isn't a directory", includePath)};
		if (Logging())
			std::println("Add include search path: \"{}\"", path.string());
		spAddSearchPath(slangRequest, path.generic_string().c_str());
	}

	spSetDebugInfoLevel(slangRequest, static_cast<SlangDebugInfoLevel>(request.debugInfoLevel));
	spSetDebugInfoFormat(slangRequest, static_cast<SlangDebugInfoFormat>(request.debugInfoFormat));
	spSetOptimizationLevel(slangRequest, static_cast<SlangOptimizationLevel>(request.optimizationLevel));
	spSetMatrixLayoutMode(slangRequest, static_cast<SlangMatrixLayoutMode>(request.matrixLayoutMode));

	for (const auto& [key, value] : request.definitions)
		spAddPreprocessorDefine(slangRequest, key.c_str(), value.c_str());
	// what a format's target lacks or has otherwise, for the shaders to take into account (slang defines nothing per target:
	// __HLSL__ is every target's, the language's): D3D has no point size, its points always a pixel; and its clip space is
	// y up, where rhi's is Vulkan's, y down (gfx/shaders/clip.h)
	if (request.format == rhi::ShaderFormat::kDxil)
	{
		spAddPreprocessorDefine(slangRequest, "SHADER_TARGET_NO_POINT_SIZE", "1");
		spAddPreprocessorDefine(slangRequest, "SHADER_TARGET_CLIP_Y_UP", "1");
	}
	if (specialization)
		spAddPreprocessorDefine(slangRequest, kSpecializationDefinition, std::format("{}u", *specialization).c_str());

	// the code's target. for a format other than spir-v, spir-v first all the same: the reflection is the first target's,
	// and rhi's layouts are its descriptor sets and bindings (slang reflects Metal's argument buffers otherwise), which
	// the other backends map their own way (see docs/backends-plan.md); the code is then the second target's
	if (reflectionTarget && request.format != rhi::ShaderFormat::kSpirv16)
	{
		auto [target, profile] = SlangTarget(rhi::ShaderFormat::kSpirv16);
		int reflectionIndex = spAddCodeGenTarget(slangRequest, target);
		spSetTargetProfile(slangRequest, reflectionIndex, spFindProfile(gSession, profile));
	}
	auto [target, targetProfile] = SlangTarget(request.format);
	const int codeTargetIndex = spAddCodeGenTarget(slangRequest, target);
	if (*targetProfile != '\0')
		spSetTargetProfile(slangRequest, codeTargetIndex, spFindProfile(gSession, targetProfile));

	// keep the entry points' names in the binaries (rhi::EntryPoint names them): for spir-v, slang otherwise renames every
	// entry point to "main"
	// and for DXIL each descriptor set (a parameter block, see bindings.slang) a register space of its own, D3D12's
	// descriptor tables (slang packs them all into one space otherwise)
	std::vector<const char*> options{"-fvk-use-entrypoint-name"};
	if (request.format == rhi::ShaderFormat::kDxil)
		options.push_back("-parameter-blocks-use-register-spaces");
	if (SLANG_FAILED(spProcessCommandLineArguments(slangRequest, options.data(), static_cast<int>(options.size()))))
		return {-1, "failed to set slang's options"};
	return {codeTargetIndex, {}};
}

// whether spir-v decorates a specialization constant SpecId 0 (OpDecorate, 71, of SpecId, 1, the literal 0), after its
// header of 5 words
bool ReadsSpecializationConstant(std::span<const uint32_t> words)
{
	constexpr uint32_t kHeaderWordCount = 5;
	constexpr uint32_t kOpDecorate = 71;
	constexpr uint32_t kDecorationSpecId = 1;
	for (size_t at = kHeaderWordCount; at < words.size();)
	{
		const uint32_t wordCount = words[at] >> 16U;
		const uint32_t opcode = words[at] & 0xFFFFU;
		if (wordCount == 0 || at + wordCount > words.size())
			return false;
		if (opcode == kOpDecorate && wordCount >= 4 && words[at + 2] == kDecorationSpecId && words[at + 3] == 0)
			return true;
		at += wordCount;
	}
	return false;
}

// DXIL's specializations (a format without specialization constants, see kSpecializationDefinition): the entry points
// that read the constant (the global uniforms' implicit constant buffer in their DXIL; found by their spir-v, see below)
// compiled again for each value of the request's specializations and 0, the constant a literal, so
// that DXC folds it and drops the code it disables. their binaries for 0 replace theirs (no binary then reads $Globals:
// the push constants keep their register, b0, which the shaders give them explicitly, see bindings.slang), the others
// are the set's specializations. an error where the constant's values compile to other layouts, or something else is a
// global uniform
std::string CompileSpecializations(
	const Request& request, SlangCompileRequest* compiled, int codeTargetIndex, slang::ShaderReflection* codeLayout, rhi::ShaderSet& shaderSet)
{
	if (codeLayout->getGlobalConstantBufferSize() == 0)
		return {};
	// the entry points that read it: their spir-v (the reflection target's, the request's first) decorates a
	// specialization constant SpecId 0, where slang keeps it per entry point that uses it. (slang's entry point metadata,
	// isParameterLocationUsed, isn't there for a compile request's program: getEntryPointMetadata fails, E_INVALIDARG)
	(void)codeTargetIndex;
	std::vector<size_t> reading;
	for (size_t entryPointIt = 0; entryPointIt < request.entryPoints.size(); entryPointIt++)
	{
		ISlangBlob* blob = nullptr;
		if (SLANG_FAILED(spGetEntryPointCodeBlob(compiled, static_cast<int>(entryPointIt), 0, &blob)))
			return std::format("failed to get the spir-v of {}", request.entryPoints[entryPointIt].name);
		const bool reads = ReadsSpecializationConstant(
			std::span(static_cast<const uint32_t*>(blob->getBufferPointer()), blob->getBufferSize() / sizeof(uint32_t)));
		blob->release();
		if (reads)
			reading.push_back(entryPointIt);
	}
	if (Logging())
		std::println("SPECIALIZATIONS: {} of {} entry points read the specialization constant", reading.size(), request.entryPoints.size());
	if (reading.empty())
		return {};

	std::vector<uint32_t> values{0};
	for (uint32_t value : request.specializations)
		if (std::ranges::find(values, value) == values.end())
			values.push_back(value);
	for (uint32_t value : values)
	{
		SlangCompileRequest* slangRequest = spCreateCompileRequest(gSession);
		auto [targetIndex, error] = ConfigureRequest(slangRequest, request, value, false);
		int translationUnitIndex = spAddTranslationUnit(slangRequest, static_cast<SlangSourceLanguage>(request.sourceLanguage), nullptr);
		spAddTranslationUnitSourceFile(slangRequest, translationUnitIndex, request.file.c_str());
		for (size_t entryPointIt : reading)
		{
			const auto& [name, stage] = request.entryPoints[entryPointIt];
			spAddEntryPoint(slangRequest, translationUnitIndex, name.c_str(), static_cast<SlangStage>(stage));
		}
		if (error.empty() && SLANG_FAILED(spCompile(slangRequest)))
			error = std::format("specialization {}: {}", value, spGetDiagnosticOutput(slangRequest) != nullptr ? spGetDiagnosticOutput(slangRequest) : "");
		if (error.empty())
			if (auto* layout = slang::ShaderReflection::get(slangRequest); layout != nullptr && layout->getGlobalConstantBufferSize() > 0)
				error = std::format("specialization {}: global uniforms besides the specialization constant, which DXIL can't bind", value);
		for (size_t readingIt = 0; error.empty() && readingIt < reading.size(); readingIt++)
		{
			ISlangBlob* blob = nullptr;
			if (SLANG_FAILED(spGetEntryPointCodeBlob(slangRequest, static_cast<int>(readingIt), targetIndex, &blob)))
			{
				error = std::format("specialization {}: failed to get the code of {}", value, request.entryPoints[reading[readingIt]].name);
				break;
			}
			rhi::ShaderBinary code(blob->getBufferSize());
			std::memcpy(code.data(), blob->getBufferPointer(), blob->getBufferSize());
			blob->release();
			const auto shader = static_cast<uint32_t>(reading[readingIt]);
			if (value == 0)
				std::get<0>(shaderSet.shaders[shader]) = std::move(code);
			else
				shaderSet.specializations.push_back({.shader = shader, .value = value, .code = std::move(code)});
		}
		spDestroyCompileRequest(slangRequest);
		if (!error.empty())
			return error;
	}
	return {};
}

Response Compile(const Request& request)
{
	Response response;
	if (gSession == nullptr)
		gSession = spCreateSession();

	SlangCompileRequest* slangRequest = spCreateCompileRequest(gSession);

	if (!request.intermediatePath.empty())
	{
		auto path = std::filesystem::absolute(request.intermediatePath);
		std::error_code error;
		if (!std::filesystem::exists(path))
			std::filesystem::create_directories(path, error);
		if (error || !std::filesystem::is_directory(path))
		{
			spDestroyCompileRequest(slangRequest);
			response.diagnostics = std::format("failed to create the intermediate path {}", path.string());
			return response;
		}
		if (Logging())
			std::println("Set intermediate path: \"{}\"", path.string());
		spSetDumpIntermediatePrefix(slangRequest, (path.generic_string() + "/").c_str());
		// only where asked for: without a prefix slang dumps them into the working directory (the precompile tool, run
		// from the checkout's root, left 344 .spv files there)
		spSetDumpIntermediates(slangRequest, true);
	}

	auto [codeTargetIndex, setupError] = ConfigureRequest(slangRequest, request, std::nullopt, true);
	if (!setupError.empty())
	{
		spDestroyCompileRequest(slangRequest);
		response.diagnostics = setupError;
		return response;
	}

	int translationUnitIndex = spAddTranslationUnit(slangRequest, static_cast<SlangSourceLanguage>(request.sourceLanguage), nullptr);
	spAddTranslationUnitSourceFile(slangRequest, translationUnitIndex, request.file.c_str());

	std::vector<rhi::EntryPoint> entryPoints;
	for (const auto& [entryPoint, stage] : request.entryPoints)
	{
		if (spAddEntryPoint(slangRequest, translationUnitIndex, entryPoint.c_str(), static_cast<SlangStage>(stage)) !=
			static_cast<int>(entryPoints.size()))
		{
			spDestroyCompileRequest(slangRequest);
			response.diagnostics = std::format("failed to add the entry point {}", entryPoint);
			return response;
		}
		entryPoints.emplace_back(entryPoint, GetStage(static_cast<SlangStage>(stage)), std::nullopt);
	}

	const SlangResult compileResult = spCompile(slangRequest);
	if (const auto* diagnostics = spGetDiagnosticOutput(slangRequest))
		response.diagnostics = diagnostics;
	if (SLANG_FAILED(compileResult))
	{
		spDestroyCompileRequest(slangRequest);
		return response;
	}

	int dependencyCount = spGetDependencyFileCount(slangRequest);
	for (int dependencyIt = 0; dependencyIt < dependencyCount; dependencyIt++)
		response.dependencies.emplace_back(spGetDependencyFilePath(slangRequest, dependencyIt));

	auto& shaderSet = response.shaderSet;
	for (size_t entryPointIt = 0; entryPointIt < entryPoints.size(); entryPointIt++)
	{
		ISlangBlob* blob = nullptr;
		if (SLANG_FAILED(spGetEntryPointCodeBlob(slangRequest, static_cast<int>(entryPointIt), codeTargetIndex, &blob)))
		{
			spDestroyCompileRequest(slangRequest);
			response.diagnostics += std::format("failed to get the code of the entry point {}", std::get<0>(entryPoints[entryPointIt]));
			return response;
		}
		shaderSet.shaders.emplace_back(blob->getBufferSize(), entryPoints[entryPointIt]);
		std::memcpy(std::get<0>(shaderSet.shaders.back()).data(), blob->getBufferPointer(), blob->getBufferSize());
		blob->release();
	}

	slang::ShaderReflection* shaderReflection = slang::ShaderReflection::get(slangRequest);

	std::vector<uint32_t> genericParameterIndices(shaderReflection->getTypeParameterCount());
	uint32_t parameterBlockCounter = 0;
	for (auto parameterIndex = 0; std::cmp_less(parameterIndex, shaderReflection->getParameterCount()); parameterIndex++)
	{
		auto* parameter = shaderReflection->getParameterByIndex(parameterIndex);
		auto* typeLayout = parameter->getTypeLayout();
		if (parameter->getType()->getKind() == slang::TypeReflection::Kind::ParameterBlock)
		{
			auto parameterBlockIndex = ++parameterBlockCounter;
			auto* elementTypeLayout = typeLayout->getElementTypeLayout();
			if (elementTypeLayout && elementTypeLayout->getKind() == slang::TypeReflection::Kind::GenericTypeParameter)
				genericParameterIndices[elementTypeLayout->getGenericParamIndex()] = parameterBlockIndex;
		}
	}

	for (auto parameterIndex = 0; std::cmp_less(parameterIndex, shaderReflection->getParameterCount()); parameterIndex++)
		CreateLayoutBindings(shaderReflection->getParameterByIndex(parameterIndex), genericParameterIndices, shaderSet.layouts);

	// the buffers' data laid out the same on every backend (see CheckPackedLayout): an error otherwise, whatever the
	// format compiled for, so that a struct only Metal would misread fails Vulkan's build too
	{
		std::vector<std::string> mismatches;
		for (unsigned parameterIt = 0; parameterIt < shaderReflection->getParameterCount(); parameterIt++)
		{
			auto* parameter = shaderReflection->getParameterByIndex(parameterIt);
			CheckPackedBuffers(parameter->getTypeLayout(), parameter->getName(), mismatches);
		}
		if (!mismatches.empty())
		{
			spDestroyCompileRequest(slangRequest);
			response.diagnostics += std::format(
				"{}: buffers whose data std430 lays out otherwise than Metal's packed layout (pad them explicitly, see capi.h):",
				request.file);
			for (const auto& mismatch : mismatches)
				response.diagnostics += std::format("\n  {}", mismatch);
			return response;
		}
	}

	// a format whose code binds each set and the push constants at a slot of its own (see rhi::kNoShaderSlot): each one's,
	// from the code target's layout, whose parameters are the reflection target's in the same order
	// (DXIL's registers, a set's space and a binding's register, and its buffers' layouts checked: AssignRegisters)
	if (request.format != rhi::ShaderFormat::kSpirv16)
		if (auto error = request.format == rhi::ShaderFormat::kDxil
							 ? AssignRegisters(request, slangRequest, codeTargetIndex, shaderReflection, shaderSet)
							 : AssignSlots(slangRequest, codeTargetIndex, shaderReflection, shaderSet.layouts);
			!error.empty())
		{
			spDestroyCompileRequest(slangRequest);
			response.diagnostics += error;
			return response;
		}

	for (uint32_t entryPointIt = 0; entryPointIt < shaderReflection->getEntryPointCount(); entryPointIt++)
	{
		slang::EntryPointReflection* entryPointReflection = shaderReflection->getEntryPointByIndex(entryPointIt);
		if (entryPointReflection->getStage() != SLANG_STAGE_COMPUTE)
			continue;
		auto& launchParameters = std::get<2>(std::get<1>(shaderSet.shaders[entryPointIt])).emplace();
		entryPointReflection->getComputeThreadGroupSize(launchParameters.threadGroupSize.size(), launchParameters.threadGroupSize.data());
		entryPointReflection->getComputeWaveSize(&launchParameters.waveSize);
	}

	spDestroyCompileRequest(slangRequest);
	response.ok = true;
	return response;
}

} // namespace

} // namespace gfx::shadercompiler

SPEEDO_SHADERCOMPILER_EXPORT int SpeedoShaderCompile(
	const std::byte* requestBytes, size_t requestSize, std::byte** responseBytes, size_t* responseSize)
{
	using namespace gfx::shadercompiler;

	std::scoped_lock lock(gMutex);

	Request request;
	if (zpp::bits::failure(zpp::bits::in(std::span<const std::byte>(requestBytes, requestSize))(request)))
		return 1;

	Response response = Compile(request);

	std::vector<std::byte> data;
	if (zpp::bits::failure(zpp::bits::out(data)(response)))
		return 2;

	*responseBytes = static_cast<std::byte*>(std::malloc(data.size()));
	if (*responseBytes == nullptr)
		return 3;
	std::memcpy(*responseBytes, data.data(), data.size());
	*responseSize = data.size();
	return 0;
}

SPEEDO_SHADERCOMPILER_EXPORT void SpeedoShaderCompileFree(std::byte* response)
{
	std::free(response);
}
