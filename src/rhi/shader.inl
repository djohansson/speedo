#include <core/file.h>
#include <core/std_extra.h>

#include <zpp_bits.h>

//NOLINTBEGIN(readability-identifier-naming)
template <GraphicsApi G>
[[nodiscard]] zpp::bits::members<std_extra::member_count<DescriptorSetLayoutCreateDesc<G>>()> serialize(const DescriptorSetLayoutCreateDesc<G>&);
//NOLINTEND(readability-identifier-naming)

namespace shader
{

template <GraphicsApi G>
ShaderStageFlagBits<G> GetStageFlag(SlangStage stage);

template <GraphicsApi G>
DescriptorType<kVk> GetDescriptorType(
	slang::TypeReflection::Kind kind, SlangResourceShape shape, SlangResourceAccess access);

template <GraphicsApi G>
uint32_t CreateLayoutBindings(
	slang::VariableLayoutReflection* parameter,
	const std::vector<uint32_t>& genericParameterIndices,
	std::map<uint32_t, DescriptorSetLayoutCreateDesc<G>>& layouts,
	const unsigned* parentSpace = nullptr,
	const char* parentName = nullptr);

} // namespace shader

template <GraphicsApi G>
ShaderSet<G> ShaderLoader::Load(const std::filesystem::path& file, const SlangConfiguration& config)
{
	auto shaderSet = ShaderSet<G>{};

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

	auto loadSlang = [slangSession = myCompilerSession.get(),
					  &intermediatePath = myIntermediatePath,
					  &includePaths = myIncludePaths,
					  &shaderSet,
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
				CHECKF(!error, "Failed to create intermediate path.");
			}

			std::cout << "Set intermediate path: " << path << '\n';
			ASSERT(std::filesystem::is_directory(path));
			spSetDumpIntermediatePrefix(slangRequest, (path.generic_string() + "/").c_str());
		}

		spSetDumpIntermediates(slangRequest, true);

		for (const auto& includePath : includePaths)
		{
			auto path = std::filesystem::canonical(includePath);

			std::cout << "Add include search path: " << path << '\n';
			ASSERT(std::filesystem::is_directory(path));
			spAddSearchPath(slangRequest, path.generic_string().c_str());
		}

		spSetDebugInfoLevel(slangRequest, config.debugInfoLevel);
		spSetDebugInfoFormat(slangRequest, config.debugInfoFormat);
		spSetOptimizationLevel(slangRequest, config.optimizationLevel);
		spSetMatrixLayoutMode(slangRequest, config.matrixLayoutMode);

		//spAddPreprocessorDefine(slangRequest, "SHADERTYPES_H_CPU_TARGET", "false");

		int targetIndex = spAddCodeGenTarget(slangRequest, config.target);

		spSetTargetProfile(slangRequest, targetIndex, spFindProfile(slangSession, config.targetProfile.c_str()));
		//spSetTargetFlags(slangRequest, targetIndex, 0); SLANG_TARGET_FLAG_GENERATE_SPIRV_DIRECTLY

		int translationUnitIndex = spAddTranslationUnit(slangRequest, config.sourceLanguage, nullptr);

		spAddTranslationUnitSourceFile(
			slangRequest, translationUnitIndex, file.generic_string().c_str());

		std::vector<EntryPoint<G>> entryPoints;
		for (const auto& [ep, stage] : config.entryPoints)
		{
			CHECKF(spAddEntryPoint(slangRequest, translationUnitIndex, ep.c_str(), stage) == entryPoints.size(), "Failed to add entry point.");
			entryPoints.emplace_back("main", shader::GetStageFlag<G>(stage), std::nullopt);
		}

		const SlangResult compileRes = spCompile(slangRequest);

		if (const auto* diagnostics = spGetDiagnosticOutput(slangRequest))
			std::cout << diagnostics;

		if (SLANG_FAILED(compileRes))
		{
			spDestroyCompileRequest(slangRequest);

			CHECKF(false, "Failed to compile slang file.");
		}

		int depCount = spGetDependencyFileCount(slangRequest);
		for (int dep = 0; dep < depCount; dep++)
		{
			char const* depPath = spGetDependencyFilePath(slangRequest, dep);
			// ... todo: add dependencies for recompile & hot reload
			std::cout << "File include/import: " << depPath << '\n';
		}

		for (const auto& entryPoint : entryPoints)
		{
			ISlangBlob* blob = nullptr;
			if (SLANG_FAILED(
					spGetEntryPointCodeBlob(slangRequest, &entryPoint - entryPoints.data(), 0, &blob)))
			{
				spDestroyCompileRequest(slangRequest);

				CHECKF(false, "Failed to get slang blob.");
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
			shader::CreateLayoutBindings<G>(
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
	params.append("slang-0.9.3"); // todo: read version from slang header
	params.append(config.ToString());
	static constexpr size_t kSha2Size = 32;
	std::array<uint8_t, kSha2Size> sha2;
	picosha2::hash256(params.cbegin(), params.cend(), sha2.begin(), sha2.end());
	picosha2::bytes_to_hex_string(sha2.cbegin(), sha2.cend(), paramsHash);
	file::LoadAsset(file, loadSlang, loadBin, saveBin, paramsHash);

	CHECKF(!shaderSet.shaders.empty(), "Failed to load shaders.");

	return shaderSet;
}
