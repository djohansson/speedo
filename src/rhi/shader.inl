#include <core/file.h>

#include <cereal/archives/binary.hpp>
//#include <cereal/archives/json.hpp>

namespace shader
{

template <GraphicsBackend B>
ShaderStageFlagBits<B> getStageFlags(SlangStage stage);

template <GraphicsBackend B>
DescriptorType<Vk> getDescriptorType(
	slang::TypeReflection::Kind kind, SlangResourceShape shape, SlangResourceAccess access);

template <GraphicsBackend B>
uint32_t createLayoutBindings(
	slang::VariableLayoutReflection* parameter,
	const std::vector<uint32_t>& genericParameterIndices,
	std::map<uint32_t, DescriptorSetLayoutCreateDesc<B>>& layouts,
	const unsigned* parentSpace = nullptr,
	const char* parentName = nullptr);

} // namespace shader

template <GraphicsBackend B>
ShaderSet<B> ShaderLoader::load(const std::filesystem::path& slangFile)
{
	auto shaderSet = ShaderSet<B>{};

	auto loadBin = [&shaderSet](std::istream& stream)
	{
		cereal::BinaryInputArchive bin(stream);
		//cereal::JSONInputArchive bin(stream);
		bin(shaderSet);

		return true;
	};

	auto saveBin = [&shaderSet](std::ostream& stream)
	{
		cereal::BinaryOutputArchive bin(stream);
		//cereal::JSONOutputArchive bin(stream);
		bin(shaderSet);

		return true;
	};

	auto loadSlang = [slangSession = myCompilerSession.get(),
					  &intermediatePath = myIntermediatePath,
					  &includePaths = myIncludePaths,
					  &shaderSet,
					  &slangFile](std::istream& stream)
	{
		constexpr bool useGLSL = true;

		SlangCompileRequest* slangRequest = spCreateCompileRequest(slangSession);

		if (intermediatePath)
		{
			auto path = std::filesystem::absolute(intermediatePath.value());

			if (!std::filesystem::exists(path))
				std::filesystem::create_directory(path);

			std::cout << "Set intermediate path: " << path << '\n';
			assert(std::filesystem::is_directory(path));
			spSetDumpIntermediatePrefix(slangRequest, (path.generic_string() + "/").c_str());
		}

		spSetDumpIntermediates(slangRequest, true);

		for (const auto& includePath : includePaths)
		{
			auto path = std::filesystem::canonical(includePath);

			std::cout << "Add include search path: " << path << '\n';
			assert(std::filesystem::is_directory(path));
			spAddSearchPath(slangRequest, path.generic_string().c_str());
		}

		spSetDebugInfoLevel(slangRequest, SLANG_DEBUG_INFO_LEVEL_MAXIMAL);
		spSetOptimizationLevel(slangRequest, SLANG_OPTIMIZATION_LEVEL_MAXIMAL);

		//spAddPreprocessorDefine(slangRequest, "SHADERTYPES_H_CPU_TARGET", "false");

		int targetIndex = spAddCodeGenTarget(slangRequest, SLANG_SPIRV);

		if (useGLSL)
			spSetTargetProfile(slangRequest, targetIndex, spFindProfile(slangSession, "GLSL_460"));
		else
			spSetTargetProfile(slangRequest, targetIndex, spFindProfile(slangSession, "sm_6_5"));

		// spSetTargetFlags(
		// 	slangRequest,
		// 	targetIndex,
		// 	SLANG_TARGET_FLAG_VK_USE_SCALAR_LAYOUT); //todo: remove vk dep?

		int translationUnitIndex =
			spAddTranslationUnit(slangRequest, SLANG_SOURCE_LANGUAGE_SLANG, nullptr);

		spAddTranslationUnitSourceFile(
			slangRequest, translationUnitIndex, slangFile.generic_string().c_str());

		// temp
		constexpr const char* epStrings[]{
			"vertexMain",
			"fragmentMain",
			"computeMain",
		};
		constexpr const SlangStage epStages[]{
			SLANG_STAGE_VERTEX,
			SLANG_STAGE_FRAGMENT,
			SLANG_STAGE_COMPUTE,
		};
		// end temp

		static_assert(std::ssize(epStrings) == std::ssize(epStages));

		std::vector<EntryPoint<B>> entryPoints;
		for (int i = 0; i < std::ssize(epStrings); i++)
		{
			int index =
				spAddEntryPoint(slangRequest, translationUnitIndex, epStrings[i], epStages[i]);

			if (index != entryPoints.size())
				throw std::runtime_error("Failed to add entry point.");

			entryPoints.push_back(std::make_pair(
				useGLSL ? "main" : epStrings[i], shader::getStageFlags<B>(epStages[i])));
		}

		const SlangResult compileRes = spCompile(slangRequest);

		if (auto diagnostics = spGetDiagnosticOutput(slangRequest))
			std::cout << diagnostics;

		if (SLANG_FAILED(compileRes))
		{
			spDestroyCompileRequest(slangRequest);

			throw std::runtime_error("Failed to compile slang shader module.");
		}

		int depCount = spGetDependencyFileCount(slangRequest);
		for (int dep = 0; dep < depCount; dep++)
		{
			char const* depPath = spGetDependencyFilePath(slangRequest, dep);
			// ... todo: add dependencies for recompile & hot reload
			std::cout << "File include/import: " << depPath << '\n';
		}

		for (const auto& ep : entryPoints)
		{
			ISlangBlob* blob = nullptr;
			//if (SLANG_FAILED(
					spGetEntryPointCodeBlob(slangRequest, &ep - &entryPoints[0], 0, &blob);
			//))
			//{
			//	spDestroyCompileRequest(slangRequest);

			//	throw std::runtime_error("Failed to get slang blob.");
			//}

			shaderSet.shaders.emplace_back(std::make_tuple(blob->getBufferSize(), ep));
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
			auto parameter = shaderReflection->getParameterByIndex(parameterIndex);
			auto* typeLayout = parameter->getTypeLayout();

			if (parameter->getType()->getKind() == slang::TypeReflection::Kind::ParameterBlock)
			{
				auto parameterBlockIndex = ++parameterBlockCounter;
				auto* elementTypeLayout = typeLayout->getElementTypeLayout();
				auto kind = elementTypeLayout->getKind();
				if (kind == slang::TypeReflection::Kind::GenericTypeParameter)
				{
					auto genericParamIndex = elementTypeLayout->getGenericParamIndex();
					genericParameterIndices[genericParamIndex] = parameterBlockIndex;
				}
			}
		}

		for (auto parameterIndex = 0; parameterIndex < shaderReflection->getParameterCount();
			 parameterIndex++)
			shader::createLayoutBindings<B>(
				shaderReflection->getParameterByIndex(parameterIndex),
				genericParameterIndices,
				shaderSet.layouts);

		// for (uint32_t epIndex = 0; epIndex < shaderReflection->getEntryPointCount(); epIndex++)
		// {
		// 	slang::EntryPointReflection* epReflection = shaderReflection->getEntryPointByIndex(epIndex);
		// SlangUInt threadGroupSize[3];
		// epReflection->getComputeThreadGruopSize(3, &threadGroupSize[0]);

		// 	for (unsigned parameterIndex = 0; parameterIndex < epReflection->getParameterCount(); parameterIndex++)
		// 		shader::createLayoutBindings<B>(epReflection->getParameterByIndex(pp), shaderSet->layouts);
		// }

		spDestroyCompileRequest(slangRequest);

		return true;
	};

	static constexpr char loaderTypeStr[] = "slang";
	static constexpr char loaderVersionStr[] = "0.9.1-dev";
	loadCachedSourceFile<loaderTypeStr, loaderVersionStr>(
		slangFile, slangFile, loadSlang, loadBin, saveBin);

	if (shaderSet.shaders.empty())
		throw std::runtime_error("Failed to load shaders.");

	return shaderSet;
}

#include "vulkan/shader.inl"
