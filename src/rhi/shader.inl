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
ShaderStageFlagBits<G> GetStageFlags(SlangStage stage);

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
ShaderSet<G> ShaderLoader::Load(const std::filesystem::path& slangFile)
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
					  &slangFile](auto& /*todo: use me: in*/) -> std::error_code
	{
		constexpr bool kUseGlsl = true;

		SlangCompileRequest* slangRequest = spCreateCompileRequest(slangSession);

		if (intermediatePath)
		{
			auto path = std::filesystem::absolute(intermediatePath.value());

			if (!std::filesystem::exists(path))
				std::filesystem::create_directory(path);

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

		spSetDebugInfoLevel(slangRequest, SLANG_DEBUG_INFO_LEVEL_STANDARD);
		spSetOptimizationLevel(slangRequest, SLANG_OPTIMIZATION_LEVEL_MAXIMAL);

		//spAddPreprocessorDefine(slangRequest, "SHADERTYPES_H_CPU_TARGET", "false");

		int targetIndex = spAddCodeGenTarget(slangRequest, SLANG_SPIRV);

		if (kUseGlsl)
			spSetTargetProfile(slangRequest, targetIndex, spFindProfile(slangSession, "GLSL_460"));
		else
			spSetTargetProfile(slangRequest, targetIndex, spFindProfile(slangSession, "sm_6_5"));

		spSetMatrixLayoutMode(slangRequest, SLANG_MATRIX_LAYOUT_COLUMN_MAJOR);

		// spSetTargetFlags(
		// 	slangRequest,
		// 	targetIndex,
		// 	SLANG_TARGET_FLAG_VK_USE_SCALAR_LAYOUT); //todo: remove vk dep?

		int translationUnitIndex =
			spAddTranslationUnit(slangRequest, SLANG_SOURCE_LANGUAGE_SLANG, nullptr);

		spAddTranslationUnitSourceFile(
			slangRequest, translationUnitIndex, slangFile.generic_string().c_str());

		// temp
		static constexpr const auto kEpStrings = std::to_array<std::string_view>({
			"VertexMain",
			"FragmentMain",
			"ComputeMain"
		});
		static constexpr const auto kEpStages = std::to_array<SlangStage>({
			SLANG_STAGE_VERTEX,
			SLANG_STAGE_FRAGMENT,
			SLANG_STAGE_COMPUTE
		});
		static_assert(std::size(kEpStrings) == std::size(kEpStages));
		// end temp

		std::vector<EntryPoint<G>> entryPoints;
		for (unsigned i = 0; i < std::size(kEpStrings); i++)
		{
			int index =
				spAddEntryPoint(slangRequest, translationUnitIndex, kEpStrings[i].data(), kEpStages[i]);

			CHECKF(index == entryPoints.size(), "Failed to add entry point.");

			entryPoints.push_back(std::make_pair(
				kUseGlsl ? "main" : kEpStrings[i].data(), shader::GetStageFlags<G>(kEpStages[i])));
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

		// for (uint32_t epIndex = 0; epIndex < shaderReflection->GetEntryPointCount(); epIndex++)
		// {
		// 	slang::EntryPointReflection* epReflection = shaderReflection->GetEntryPointByIndex(epIndex);
		// SlangUInt threadGroupSize[3];
		// epReflection->getComputeThreadGruopSize(3, &threadGroupSize[0]);

		// 	for (unsigned parameterIndex = 0; parameterIndex < epReflection->getParameterCount(); parameterIndex++)
		// 		shader::CreateLayoutBindings<G>(epReflection->getParameterByIndex(pp), shaderSet->layouts);
		// }

		spDestroyCompileRequest(slangRequest);

		return {};
	};

	file::LoadAsset<
		std_extra::make_string_literal<"slang">().data(),
		std_extra::make_string_literal<"0.9.2">().data()>(slangFile, loadSlang, loadBin, saveBin);

	CHECKF(!shaderSet.shaders.empty(), "Failed to load shaders.");

	return shaderSet;
}
