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

#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <mutex>
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

	for (auto categoryIndex = 0; std::cmp_less(categoryIndex, categoryCount); categoryIndex++)
	{
		auto subCategory = parameter->getCategoryByIndex(categoryIndex);

		if (subCategory == slang::ParameterCategory::DescriptorTableSlot ||
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

// one compile at a time (slang isn't thread safe, even across sessions: see gfx::ShaderLoader), and the session made at
// the first, kept until the process ends
std::mutex gMutex;
SlangSession* gSession = nullptr;

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

	for (const auto& includePath : request.includePaths)
	{
		std::error_code error;
		auto path = std::filesystem::canonical(includePath, error);
		if (error || !std::filesystem::is_directory(path))
		{
			spDestroyCompileRequest(slangRequest);
			response.diagnostics = std::format("the include path {} isn't a directory", includePath);
			return response;
		}
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

	auto [target, targetProfile] = SlangTarget(request.format);
	int targetIndex = spAddCodeGenTarget(slangRequest, target);
	spSetTargetProfile(slangRequest, targetIndex, spFindProfile(gSession, targetProfile));

	// keep the entry points' names in the binaries (rhi::EntryPoint names them): for spir-v, slang otherwise renames every
	// entry point to "main"
	constexpr std::array<const char*, 1> kKeepEntryPointNames{"-fvk-use-entrypoint-name"};
	if (SLANG_FAILED(spProcessCommandLineArguments(slangRequest, kKeepEntryPointNames.data(), kKeepEntryPointNames.size())))
	{
		spDestroyCompileRequest(slangRequest);
		response.diagnostics = "failed to set slang's options";
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
		if (SLANG_FAILED(spGetEntryPointCodeBlob(slangRequest, static_cast<int>(entryPointIt), 0, &blob)))
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
