#include "file.h"

#include <cereal/archives/binary.hpp>
#include <cereal/cereal.hpp>
#include <cereal/types/map.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/utility.hpp>
#include <cereal/types/vector.hpp>

template <class Archive, GraphicsBackend B>
void serialize(Archive& archive, SerializableShaderReflectionModule<B>& module)
{
	archive(cereal::make_nvp("shaders", module.shaders));
	archive(cereal::make_nvp("bindings", module.bindings));
}

template <GraphicsBackend B>
std::shared_ptr<SerializableShaderReflectionModule<B>> loadSlangShaders(
	const std::filesystem::path& compilerPath,
	const std::filesystem::path& slangFile)
{
	auto slangModule = std::make_shared<SerializableShaderReflectionModule<B>>();

	auto loadBin = [&slangModule](std::istream& stream) {
		cereal::BinaryInputArchive bin(stream);
		bin(*slangModule);
	};

	auto saveBin = [&slangModule](std::ostream& stream) {
		cereal::BinaryOutputArchive bin(stream);
		bin(*slangModule);
	};

	auto loadSlang = [&slangModule, &compilerPath, &slangFile](std::istream& stream) {
		SlangSession* slangSession = spCreateSession(NULL);
		
		slangSession->setDownstreamCompilerPath(SLANG_PASS_THROUGH_DXC, compilerPath.generic_string().c_str());
		slangSession->setDefaultDownstreamCompiler(SLANG_SOURCE_LANGUAGE_HLSL, SLANG_PASS_THROUGH_DXC);
		
		SlangCompileRequest* slangRequest = spCreateCompileRequest(slangSession);

		spSetDumpIntermediates(slangRequest, true);
		spSetOptimizationLevel(slangRequest, SLANG_OPTIMIZATION_LEVEL_MAXIMAL);

		int targetIndex = spAddCodeGenTarget(slangRequest, SLANG_SPIRV);
		
		spSetTargetProfile(slangRequest, targetIndex, spFindProfile(slangSession, "sm_6_5"));
		spSetTargetFlags(slangRequest, targetIndex, SLANG_TARGET_FLAG_VK_USE_SCALAR_LAYOUT);
		
		int translationUnitIndex = spAddTranslationUnit(slangRequest, SLANG_SOURCE_LANGUAGE_SLANG, nullptr);

		std::string shaderString(
			(std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());

		spAddTranslationUnitSourceStringSpan(
			slangRequest, translationUnitIndex, std::filesystem::absolute(slangFile).generic_string().c_str(),
			shaderString.c_str(), shaderString.c_str() + shaderString.size());

		// temp
		const char* epStrings[] = {"vertexMain", "fragmentMain", "computeMain"};
		const SlangStage epStages[] = {SLANG_STAGE_VERTEX, SLANG_STAGE_FRAGMENT, SLANG_STAGE_COMPUTE};
		// end temp

		static_assert(sizeof_array(epStrings) == sizeof_array(epStages));

		std::vector<EntryPoint> entryPoints;
		for (int i = 0; i < sizeof_array(epStrings); i++)
		{
			int index =
				spAddEntryPoint(slangRequest, translationUnitIndex, epStrings[i], epStages[i]);

			if (index != entryPoints.size())
				throw std::runtime_error("Failed to add entry point.");

			entryPoints.push_back(std::make_pair(epStrings[i], epStages[i]));
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
			// ... todo: add dependencies for hot reload
			std::cout << depPath << std::endl;
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
				static_cast<const std::byte*>(blob->getBufferPointer()),
				static_cast<const std::byte*>(blob->getBufferPointer()) + blob->getBufferSize(),
				slangModule->shaders.back().first.data());
			blob->release();
		}

		slang::ShaderReflection* shaderReflection = slang::ShaderReflection::get(slangRequest);

		for (unsigned pp = 0; pp < shaderReflection->getParameterCount(); pp++)
			createSlangLayoutBindings<Vk>(shaderReflection->getParameterByIndex(pp), slangModule->bindings);

		for (uint32_t epIndex = 0; epIndex < shaderReflection->getEntryPointCount(); epIndex++)
		{
			slang::EntryPointReflection* epReflection = shaderReflection->getEntryPointByIndex(epIndex);

			for (unsigned pp = 0; pp < epReflection->getParameterCount(); pp++)
				createSlangLayoutBindings<Vk>(epReflection->getParameterByIndex(pp), slangModule->bindings);
		}

		spDestroyCompileRequest(slangRequest);
		spDestroySession(slangSession);
	};

	loadCachedSourceFile(
		slangFile, slangFile, "slang", "1.0.0-dev", loadSlang, loadBin, saveBin);

	if (slangModule->shaders.empty())
		throw std::runtime_error("Failed to load shaders.");

	return slangModule;
}
