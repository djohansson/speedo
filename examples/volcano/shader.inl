#include "file.h"

#include <cereal/archives/binary.hpp>
//#include <cereal/archives/json.hpp>

#include <slang.h>

namespace shader
{

template <GraphicsBackend B>
ShaderStageFlagBits<B> getStageFlags(SlangStage stage);

template <GraphicsBackend B>
DescriptorType<Vk> getDescriptorType(slang::TypeReflection::Kind kind, SlangResourceShape shape);

template <GraphicsBackend B>
uint32_t createLayoutBindings(
	slang::VariableLayoutReflection* parameter,
	const std::vector<uint32_t>& genericParameterIndices,
	std::map<uint32_t, DescriptorSetLayoutCreateDesc<B>>& layouts,
	const unsigned* parentSpace = nullptr,
	const char* parentName = nullptr);

template <GraphicsBackend B>
std::shared_ptr<ShaderReflectionInfo<B>> loadSlangShaders(
	const std::filesystem::path& slangFile,
	const std::vector<std::filesystem::path>& includePaths,
	const std::optional<std::filesystem::path>& compilerPath,
	const std::optional<std::filesystem::path>& intermediatePath)
{
	auto slangModule = std::make_shared<ShaderReflectionInfo<B>>();

	auto loadBin = [&slangModule](std::istream& stream)
	{
		cereal::BinaryInputArchive bin(stream);
		//cereal::JSONInputArchive bin(stream);
		bin(*slangModule);

		return true;
	};

	auto saveBin = [&slangModule](std::ostream& stream)
	{
		cereal::BinaryOutputArchive bin(stream);
		//cereal::JSONOutputArchive bin(stream);
		bin(*slangModule);

		return true;
	};

	auto loadSlang = [&slangModule, &slangFile, &includePaths, &compilerPath, &intermediatePath](
		std::istream& stream)
	{
		constexpr bool useGLSL = true;

		SlangSession* slangSession = spCreateSession(NULL);
		
		// todo: support all compilers properly.
		if (compilerPath)
		{
			auto path = std::filesystem::canonical(compilerPath.value());

			std::cout << "Set downstream compiler path: " << path << std::endl;
			assert(std::filesystem::is_directory(path));
			
			slangSession->setDownstreamCompilerPath(SLANG_PASS_THROUGH_DXC, path.generic_string().c_str());
		}

		slangSession->setDefaultDownstreamCompiler(SLANG_SOURCE_LANGUAGE_HLSL, SLANG_PASS_THROUGH_DXC);
		
		SlangCompileRequest* slangRequest = spCreateCompileRequest(slangSession);

		if (intermediatePath)
		{
			auto path = std::filesystem::canonical(intermediatePath.value());

			if (!std::filesystem::exists(path))
        		std::filesystem::create_directory(path);

			std::cout << "Set intermediate path: " << path << std::endl;
			assert(std::filesystem::is_directory(path));
			spSetDumpIntermediatePrefix(
				slangRequest,
				(path.generic_string() + "/").c_str());
		}

		spSetDumpIntermediates(slangRequest, true);

		for (const auto& includePath : includePaths)
		{
			auto path = std::filesystem::canonical(includePath);

			std::cout << "Add include search path: " << path << std::endl;
			assert(std::filesystem::is_directory(path));
			spAddSearchPath(slangRequest, path.generic_string().c_str());
		}

		spSetDebugInfoLevel(slangRequest, SLANG_DEBUG_INFO_LEVEL_MAXIMAL);
		spSetOptimizationLevel(slangRequest, SLANG_OPTIMIZATION_LEVEL_MAXIMAL);

		spAddPreprocessorDefine(slangRequest, "GPU", "true");

		int targetIndex = spAddCodeGenTarget(slangRequest, SLANG_SPIRV);

		if (useGLSL)
			spSetTargetProfile(slangRequest, targetIndex, spFindProfile(slangSession, "GLSL_460"));
		else
			spSetTargetProfile(slangRequest, targetIndex, spFindProfile(slangSession, "sm_6_5"));

		spSetTargetFlags(slangRequest, targetIndex, SLANG_TARGET_FLAG_VK_USE_SCALAR_LAYOUT); //todo: remove vk dep?
		
		int translationUnitIndex = spAddTranslationUnit(slangRequest, SLANG_SOURCE_LANGUAGE_SLANG, nullptr);

		spAddTranslationUnitSourceFile(slangRequest, translationUnitIndex, slangFile.generic_string().c_str());

		// temp
		const char* epStrings[] = {
			"vertexMain",
			"fragmentMain",
			//"computeMain",
		};
		const SlangStage epStages[] = {
			SLANG_STAGE_VERTEX,
			SLANG_STAGE_FRAGMENT,
			//SLANG_STAGE_COMPUTE,
		};
		// end temp

		static_assert(sizeof_array(epStrings) == sizeof_array(epStages));

		std::vector<EntryPoint<B>> entryPoints;
		for (int i = 0; i < sizeof_array(epStrings); i++)
		{
			int index = spAddEntryPoint(
				slangRequest,
				translationUnitIndex,
				epStrings[i],
				epStages[i]);

			if (index != entryPoints.size())
				throw std::runtime_error("Failed to add entry point.");

			entryPoints.push_back(std::make_pair(useGLSL ? "main" : epStrings[i], shader::getStageFlags<B>(epStages[i])));
		}

		const SlangResult compileRes = spCompile(slangRequest);

		if (auto diagnostics = spGetDiagnosticOutput(slangRequest))
			std::cout << diagnostics << std::endl;

		if (SLANG_FAILED(compileRes))
		{
			spDestroyCompileRequest(slangRequest);
			spDestroySession(slangSession);

			throw std::runtime_error("Failed to compile slang shader module.");
		}

		int depCount = spGetDependencyFileCount(slangRequest);
		for (int dep = 0; dep < depCount; dep++)
		{
			char const* depPath = spGetDependencyFilePath(slangRequest, dep);
			// ... todo: add dependencies for recompile & hot reload
			std::cout << "File include/import: " << depPath << std::endl;
		}

		for (const auto& ep : entryPoints)
		{
			ISlangBlob* blob = nullptr;
			if (SLANG_FAILED(spGetEntryPointCodeBlob(slangRequest, &ep - &entryPoints[0], 0, &blob)))
			{
				spDestroyCompileRequest(slangRequest);
				spDestroySession(slangSession);

				throw std::runtime_error("Failed to get slang blob.");
			}

			slangModule->shaders.emplace_back(std::make_pair(blob->getBufferSize(), ep));
			std::copy(
				static_cast<const char*>(blob->getBufferPointer()),
				static_cast<const char*>(blob->getBufferPointer()) + blob->getBufferSize(),
				slangModule->shaders.back().first.data());
			blob->release();
		}

		slang::ShaderReflection* shaderReflection = slang::ShaderReflection::get(slangRequest);

		std::vector<uint32_t> genericParameterIndices(shaderReflection->getTypeParameterCount());
		uint32_t parameterBlockCounter = 0;
		for (auto parameterIndex = 0; parameterIndex < shaderReflection->getParameterCount(); parameterIndex++)
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

		for (auto parameterIndex = 0; parameterIndex < shaderReflection->getParameterCount(); parameterIndex++)
			shader::createLayoutBindings<B>(
				shaderReflection->getParameterByIndex(parameterIndex),
				genericParameterIndices,
				slangModule->layouts);

		// for (uint32_t epIndex = 0; epIndex < shaderReflection->getEntryPointCount(); epIndex++)
		// {
		// 	slang::EntryPointReflection* epReflection = shaderReflection->getEntryPointByIndex(epIndex);
		// SlangUInt threadGroupSize[3];
		// epReflection->getComputeThreadGruopSize(3, &threadGroupSize[0]);

		// 	for (unsigned parameterIndex = 0; parameterIndex < epReflection->getParameterCount(); parameterIndex++)
		// 		shader::createLayoutBindings<B>(epReflection->getParameterByIndex(pp), slangModule->layouts);
		// }

		spDestroyCompileRequest(slangRequest);
		spDestroySession(slangSession);

		return true;
	};

	loadCachedSourceFile(
		slangFile, slangFile, "slang", "0.9.1-dev", loadSlang, loadBin, saveBin);

	if (slangModule->shaders.empty())
		throw std::runtime_error("Failed to load shaders.");

	return slangModule;
}

}
