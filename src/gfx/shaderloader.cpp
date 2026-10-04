#include <gfx/shaderloader.h>

#include <core/assert.h>
#include <core/file.h>
#include <rhi/rhi.h>

#include <xxhash.h>

#include <format>
#include <iostream>
#include <utility>

namespace gfx
{

namespace shaderloader
{

// the slang target and profile that produce shaders of format
static std::tuple<SlangCompileTarget, const char*> SlangTarget(rhi::ShaderFormat format)
{
	switch (format)
	{
	case rhi::ShaderFormat::kSpirv16: return {SLANG_SPIRV, "SPIRV_1_6"};
	}
	ASSERT(false);
	return {SLANG_TARGET_UNKNOWN, ""};
}

static rhi::ShaderStage GetStage(SlangStage stage)
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

static rhi::DescriptorType GetDescriptorType(
	slang::TypeReflection::Kind kind, SlangResourceShape shape, SlangResourceAccess access)
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

static void AddBinding(
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

	std::cout << "ADD BINDING \"" << name << "\": Set: " << bindingSpace
			  << ", Binding: " << binding.binding << ", Count: " << descriptorCount
			  << ", Size: " << sizeBytes << '\n';
}

static uint32_t CreateLayoutBindings(
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
	//ENSURE(elementTypeLayout != nullptr);
	auto elementFieldCount =
		(elementTypeLayout != nullptr) ? elementTypeLayout->getFieldCount() : 0;

	auto category = parameter->getCategory();
	const auto* typeName = typeLayout->getName();
	ENSURE(typeName != nullptr);
	auto kind = typeLayout->getKind();
	auto* type = typeLayout->getType();
	ENSURE(type != nullptr);
	auto userAttributeCount = type->getUserAttributeCount();
	auto elementKind = (elementTypeLayout != nullptr) ? elementTypeLayout->getKind()
													  : slang::TypeReflection::Kind::None;
	auto genericParamIndex =
		(elementTypeLayout != nullptr) ? elementTypeLayout->getGenericParamIndex() : 0;

	std::string fullName;
	if (parentName != nullptr)
	{
		fullName.append(parentName);
		fullName.append(".");
	}
	fullName.append(name);

	auto bindingSpace = ((parentSpace != nullptr) ? *parentSpace : space);

	uint32_t uniformsTotalSize = 0;

	for (auto categoryIndex = 0; categoryIndex < categoryCount; categoryIndex++)
	{
		auto subCategory = static_cast<SlangParameterCategory>(parameter->getCategoryByIndex(categoryIndex));
		auto spaceForCategory = parameter->getBindingSpace(subCategory);
		auto elementSize =
			(elementTypeLayout != nullptr) ? elementTypeLayout->getSize(subCategory) : 0;
		auto elementAlignment =
			(elementTypeLayout != nullptr) ? elementTypeLayout->getAlignment(subCategory) : 0;
		auto size = typeLayout->getSize(subCategory);
		auto alignment = typeLayout->getAlignment(subCategory);

		auto offsetForCategory = parameter->getOffset(subCategory);
		auto elementStride =
			(elementTypeLayout != nullptr) ? elementTypeLayout->getElementStride(subCategory) : 0;

		std::cout << "DEBUG: name: " << name << ", fullName: " << fullName << ", space: " << space
				  << ", parent space: "
				  << ((parentSpace != nullptr) ? std::to_string(*parentSpace) : "(nullptr)")
				  << ", index: " << index << ", stage: " << stage
				  << ", kind: " << static_cast<int>(kind)
				  << ", typeName: " << ((typeName != nullptr) ? typeName : "(nullptr)")
				  << ", userAttributeCount: " << userAttributeCount;

		std::cout << ", arrayElementCount: " << arrayElementCount << ", fieldCount: " << fieldCount;

		std::cout << ", category: " << category << ", subCategory: " << subCategory
				  << ", spaceForCategory: " << spaceForCategory
				  << ", offsetForCategory: " << offsetForCategory
				  << ", elementStride: " << elementStride << ", elementSize: " << elementSize
				  << ", elementAlignment: " << elementAlignment
				  << ", elementKind: " << static_cast<int>(elementKind)
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
		auto* elementField = (elementTypeLayout != nullptr)
								 ? elementTypeLayout->getFieldByIndex(elementFieldIndex)
								 : nullptr;
		//ENSURE(elementField != nullptr);
		auto* elementFieldType =
			(elementField != nullptr) ? elementField->getTypeLayout() : nullptr;
		//ENSURE(elementFieldType != nullptr);
		auto count = (elementFieldType != nullptr) && elementFieldType->isArray()
						 ? elementFieldType->getFieldCount()
						 : 1;
		uniformsTotalSize +=
			count *
			CreateLayoutBindings(
				elementField, genericParameterIndices, layouts, &bindingSpace, fullName.c_str());
	}

	for (auto categoryIndex = 0; categoryIndex < categoryCount; categoryIndex++)
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

} // namespace shaderloader

ShaderLoader::ShaderLoader(
	std::vector<std::filesystem::path>&& includePaths,
	std::vector<DownstreamCompiler>&& downstreamCompilers,
	std::optional<std::filesystem::path>&& intermediatePath)
	: myIncludePaths(std::forward<std::vector<std::filesystem::path>>(includePaths))
	, myDownstreamCompilers(std::forward<std::vector<DownstreamCompiler>>(downstreamCompilers))
	, myIntermediatePath(std::forward<std::optional<std::filesystem::path>>(intermediatePath))
	, myCompilerSession(spCreateSession(), spDestroySession)
{
	for (const auto& [sourceLanguage, compilerId, compilerPath] : myDownstreamCompilers)
	{
		myCompilerSession->setDefaultDownstreamCompiler(sourceLanguage, compilerId);

		if (!compilerPath || compilerPath->empty())
			continue;

		auto path = std::filesystem::canonical(compilerPath.value());
		
		std::cout << "Set downstream compiler path: " << path << '\n';
		ENSURE(std::filesystem::is_directory(path));

		myCompilerSession->setDownstreamCompilerPath(compilerId, path.generic_string().c_str());
	}
}

std::string ShaderLoader::SlangConfiguration::ToString() const
{
	std::string entryPointsString;
	for (const auto& [name, stage] : entryPoints)
		entryPointsString.append(std::format("[{}, {}]", name, static_cast<SlangStageIntegral>(stage)));

	return std::format(
		"sourceLanguage: {}, entryPoints: {}, "
		"optimizationLevel: {}, debugInfoLevel: {}, debugInfoFormat: {}, matrixLayoutMode: {}",
		static_cast<SlangSourceLanguageIntegral>(sourceLanguage),
		entryPointsString,
		static_cast<SlangOptimizationLevelIntegral>(optimizationLevel),
		static_cast<SlangDebugInfoLevelIntegral>(debugInfoLevel),
		static_cast<SlangDebugInfoFormatIntegral>(debugInfoFormat),
		static_cast<SlangMatrixLayoutModeIntegral>(matrixLayoutMode));
}

rhi::ShaderSet ShaderLoader::Load(const std::filesystem::path& file, const SlangConfiguration& config)
{
	auto shaderSet = rhi::ShaderSet{};

	auto loadBin = [&shaderSet](auto& inStream) -> std::error_code
	{
		if (auto result = inStream(shaderSet); failure(result))
			return std::make_error_code(result);

		return {};
	};

	auto saveBin = [&shaderSet](auto& outStream) -> std::error_code
	{
		if (auto result = outStream(shaderSet); failure(result))
			return std::make_error_code(result);

		return {};
	};

	std::vector<std::filesystem::path> dependencies;

	auto loadSlang = [slangSession = myCompilerSession.get(),
					  &intermediatePath = myIntermediatePath,
					  &includePaths = myIncludePaths,
					  &shaderSet,
					  &dependencies,
					  &file,
					  &config](auto& /*todo: use me: in*/) -> std::error_code
	{
		SlangCompileRequest* slangRequest = spCreateCompileRequest(slangSession);

		if (intermediatePath)
		{
			auto path = std::filesystem::absolute(intermediatePath.value());

			std::error_code error;

			if (!std::filesystem::exists(path))
			{
				std::filesystem::create_directories(path, error);
				ENSUREF(!error, "Failed to create intermediate path.");
			}

			std::cout << "Set intermediate path: " << path << '\n';
			ENSURE(std::filesystem::is_directory(path));
			spSetDumpIntermediatePrefix(slangRequest, (path.generic_string() + "/").c_str());
		}

		spSetDumpIntermediates(slangRequest, true);

		for (const auto& includePath : includePaths)
		{
			auto path = std::filesystem::canonical(includePath);

			std::cout << "Add include search path: " << path << '\n';
			ENSURE(std::filesystem::is_directory(path));
			spAddSearchPath(slangRequest, path.generic_string().c_str());
		}

		spSetDebugInfoLevel(slangRequest, config.debugInfoLevel);
		spSetDebugInfoFormat(slangRequest, config.debugInfoFormat);
		spSetOptimizationLevel(slangRequest, config.optimizationLevel);
		spSetMatrixLayoutMode(slangRequest, config.matrixLayoutMode);
		
		for (const auto& [key, value] : config.preprocessorDefinitions)
			spAddPreprocessorDefine(slangRequest, key.c_str(), value.c_str());
		
		auto [target, targetProfile] = shaderloader::SlangTarget(rhi::kShaderFormat);
		int targetIndex = spAddCodeGenTarget(slangRequest, target);

		spSetTargetProfile(slangRequest, targetIndex, spFindProfile(slangSession, targetProfile));
		//spSetTargetFlags(slangRequest, targetIndex, 0); SLANG_TARGET_FLAG_GENERATE_SPIRV_DIRECTLY

		int translationUnitIndex = spAddTranslationUnit(slangRequest, config.sourceLanguage, nullptr);

		spAddTranslationUnitSourceFile(
			slangRequest, translationUnitIndex, file.generic_string().c_str());

		std::vector<rhi::EntryPoint> entryPoints;
		for (const auto& [ep, stage] : config.entryPoints)
		{
			ENSUREF(spAddEntryPoint(slangRequest, translationUnitIndex, ep.c_str(), stage) == entryPoints.size(), "Failed to add entry point.");
			entryPoints.emplace_back("main", shaderloader::GetStage(stage), std::nullopt);
		}

		const SlangResult compileRes = spCompile(slangRequest);

		if (const auto* diagnostics = spGetDiagnosticOutput(slangRequest))
			std::cout << diagnostics;

		if (SLANG_FAILED(compileRes))
		{
			spDestroyCompileRequest(slangRequest);

			ENSUREF(false, "Failed to compile slang file.");
		}

		int depCount = spGetDependencyFileCount(slangRequest);
		for (int dep = 0; dep < depCount; dep++)
		{
			char const* depPath = spGetDependencyFilePath(slangRequest, dep);
			// recorded in the asset manifest, so editing an included/imported file recompiles. todo: hot reload
			dependencies.emplace_back(depPath);
			std::cout << "File include/import: " << depPath << '\n';
		}

		for (const auto& entryPoint : entryPoints)
		{
			ISlangBlob* blob = nullptr;
			if (SLANG_FAILED(
					spGetEntryPointCodeBlob(slangRequest, &entryPoint - entryPoints.data(), 0, &blob)))
			{
				spDestroyCompileRequest(slangRequest);

				ENSUREF(false, "Failed to get slang blob.");
			}

			shaderSet.shaders.emplace_back(std::make_tuple(blob->getBufferSize(), entryPoint));
			std::copy(
				static_cast<const char*>(blob->getBufferPointer()),
				static_cast<const char*>(blob->getBufferPointer()) + blob->getBufferSize(),
				std::get<0>(shaderSet.shaders.back()).data());
			blob->release();
		}

		slang::ShaderReflection* shaderReflection = slang::ShaderReflection::get(slangRequest);

		std::vector<uint32_t> genericParameterIndices(shaderReflection->getTypeParameterCount());
		uint32_t parameterBlockCounter = 0;
		for (auto parameterIndex = 0; parameterIndex < shaderReflection->getParameterCount();
			 parameterIndex++)
		{
			auto* parameter = shaderReflection->getParameterByIndex(parameterIndex);
			auto* typeLayout = parameter->getTypeLayout();

			if (parameter->getType()->getKind() == slang::TypeReflection::Kind::ParameterBlock)
			{
				auto parameterBlockIndex = ++parameterBlockCounter;
				auto* elementTypeLayout = typeLayout->getElementTypeLayout();
				if (elementTypeLayout && elementTypeLayout->getKind() == slang::TypeReflection::Kind::GenericTypeParameter)
				{
					auto genericParamIndex = elementTypeLayout->getGenericParamIndex();
					genericParameterIndices[genericParamIndex] = parameterBlockIndex;
				}
			}
		}

		for (auto parameterIndex = 0; parameterIndex < shaderReflection->getParameterCount();
			 parameterIndex++)
			shaderloader::CreateLayoutBindings(
				shaderReflection->getParameterByIndex(parameterIndex),
				genericParameterIndices,
				shaderSet.layouts);

		for (uint32_t epIndex = 0; epIndex < shaderReflection->getEntryPointCount(); epIndex++)
		{
			slang::EntryPointReflection* epReflection = shaderReflection->getEntryPointByIndex(epIndex);
			if (epReflection->getStage() != SLANG_STAGE_COMPUTE)
				continue;

			auto& [shaderBinary, entryPoint] = shaderSet.shaders[epIndex];
			auto& [epName, epStage, epLaunchParamsOptional] = entryPoint;
			auto& epLaunchParams = epLaunchParamsOptional.emplace();
			epReflection->getComputeThreadGroupSize(epLaunchParams.threadGroupSize.size(), epLaunchParams.threadGroupSize.data());
			epReflection->getComputeWaveSize(&epLaunchParams.waveSize);
		}

		spDestroyCompileRequest(slangRequest);

		return {};
	};

	std::string params, paramsHash;
	params.append("slang-");
	params.append(spGetBuildTagString()); // the loaded slang library's version, so upgrading it recompiles
	params.append("|cache-v4"); // bump when the serialized ShaderSet layout changes, to invalidate stale caches
	params.append(std::format("|format-{}", std::to_underlying(rhi::kShaderFormat)));
	params.append(config.ToString());
	static constexpr size_t kSha2Size = 32;
	std::array<uint8_t, kSha2Size> sha2;
	picosha2::hash256(params.cbegin(), params.cend(), sha2.begin(), sha2.end());
	picosha2::bytes_to_hex_string(sha2.cbegin(), sha2.cend(), paramsHash);
	auto loadResult = core::file::LoadAsset(
		file, loadSlang, loadBin, saveBin, paramsHash, [&dependencies] { return dependencies; });

	ENSUREF(loadResult && !shaderSet.shaders.empty(), "Failed to load shaders.");

	return shaderSet;
}

} // namespace gfx
