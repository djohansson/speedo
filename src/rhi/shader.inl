#include <core/file.h>
#include <core/std_extra.h>

#include <zpp_bits.h>

namespace rhi
{

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
	core::UnorderedMap<uint32_t, DescriptorSetLayoutCreateDesc<G>>& layouts,
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
		
		int targetIndex = spAddCodeGenTarget(slangRequest, config.target);

		spSetTargetProfile(slangRequest, targetIndex, spFindProfile(slangSession, config.targetProfile.c_str()));
		//spSetTargetFlags(slangRequest, targetIndex, 0); SLANG_TARGET_FLAG_GENERATE_SPIRV_DIRECTLY

		int translationUnitIndex = spAddTranslationUnit(slangRequest, config.sourceLanguage, nullptr);

		spAddTranslationUnitSourceFile(
			slangRequest, translationUnitIndex, file.generic_string().c_str());

		std::vector<EntryPoint<G>> entryPoints;
		for (const auto& [ep, stage] : config.entryPoints)
		{
			ENSUREF(spAddEntryPoint(slangRequest, translationUnitIndex, ep.c_str(), stage) == entryPoints.size(), "Failed to add entry point.");
			entryPoints.emplace_back("main", shader::GetStageFlag<G>(stage), std::nullopt);
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
	params.append("slang-");
	params.append(spGetBuildTagString()); // the loaded slang library's version, so upgrading it recompiles
	params.append("|cache-v2"); // bump when the serialized ShaderSet layout changes, to invalidate stale caches
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

} // namespace rhi