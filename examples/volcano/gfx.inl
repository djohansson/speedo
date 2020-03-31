#include "file.h"

#include <cereal/archives/binary.hpp>
#include <cereal/archives/portable_binary.hpp>
#include <cereal/cereal.hpp>
#include <cereal/types/map.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/utility.hpp>
#include <cereal/types/vector.hpp>

#include <slang.h>

template <GraphicsBackend B>
std::shared_ptr<SerializableShaderReflectionModule<B>> loadSlangShaders(
	const std::filesystem::path& slangFile)
{
	auto slangModule = std::make_shared<SerializableShaderReflectionModule<B>>();

	auto loadPBin = [&slangModule](std::istream& stream) {
		cereal::PortableBinaryInputArchive pbin(stream);
		pbin(*slangModule);
	};

	auto savePBin = [&slangModule](std::ostream& stream) {
		cereal::PortableBinaryOutputArchive pbin(stream);
		pbin(*slangModule);
	};

	auto loadSlang = [&slangModule, &slangFile](std::istream& stream) {
		SlangSession* slangSession = spCreateSession(NULL);
		SlangCompileRequest* slangRequest = spCreateCompileRequest(slangSession);

		spSetDumpIntermediates(slangRequest, true);

		int targetIndex = spAddCodeGenTarget(slangRequest, SLANG_SPIRV);
		(void)targetIndex;
		int translationUnitIndex =
			spAddTranslationUnit(slangRequest, SLANG_SOURCE_LANGUAGE_SLANG, nullptr);

		std::string shaderString(
			(std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());

		spAddTranslationUnitSourceStringSpan(
			slangRequest, translationUnitIndex, slangFile.u8string().c_str(),
			shaderString.c_str(), shaderString.c_str() + shaderString.size());

		// temp
		const char* epStrings[] = {"vertexMain", "fragmentMain"};
		const SlangStage epStages[] = {SLANG_STAGE_VERTEX, SLANG_STAGE_FRAGMENT};
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
			if (SLANG_FAILED(
					spGetEntryPointCodeBlob(slangRequest, &ep - &entryPoints[0], 0, &blob)))
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

		auto& bindings = slangModule->bindings;

		auto createLayoutBinding = [&bindings](slang::VariableLayoutReflection* parameter) {
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
					binding.descriptorCount =
						typeLayout->isArray() ? typeLayout->getElementCount() : 1;
					binding.stageFlags =
						VK_SHADER_STAGE_ALL; // todo: have not find a good way to get a good
												// value for this yet
					binding.pImmutableSamplers = nullptr; // todo;

					switch (parameter->getStage())
					{
					case SLANG_STAGE_VERTEX:
						binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
						break;
					case SLANG_STAGE_FRAGMENT:
						binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
						break;
					case SLANG_STAGE_HULL:
						assert(false); // please implement me!
						break;
					case SLANG_STAGE_DOMAIN:
						assert(false); // please implement me!
						break;
					case SLANG_STAGE_GEOMETRY:
						assert(false); // please implement me!
						break;
					case SLANG_STAGE_COMPUTE:
						assert(false); // please implement me!
						break;
					case SLANG_STAGE_RAY_GENERATION:
						assert(false); // please implement me!
						break;
					case SLANG_STAGE_INTERSECTION:
						assert(false); // please implement me!
						break;
					case SLANG_STAGE_ANY_HIT:
						assert(false); // please implement me!
						break;
					case SLANG_STAGE_CLOSEST_HIT:
						assert(false); // please implement me!
						break;
					case SLANG_STAGE_MISS:
						assert(false); // please implement me!
						break;
					case SLANG_STAGE_CALLABLE:
						assert(false); // please implement me!
						break;
					case SLANG_STAGE_NONE:
						// assert(false); // this seems to be returned for my constant buffer.
						// investigate why. perhaps that it is reused by multiple shaders? skip
						// assert for now.
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
						assert(false); // please implement me!
						break;
					case slang::TypeReflection::Kind::Struct:
						assert(false); // please implement me!
						break;
					case slang::TypeReflection::Kind::Array:
						assert(false); // please implement me!
						break;
					case slang::TypeReflection::Kind::Matrix:
						assert(false); // please implement me!
						break;
					case slang::TypeReflection::Kind::Vector:
						assert(false); // please implement me!
						break;
					case slang::TypeReflection::Kind::Scalar:
						assert(false); // please implement me!
						break;
					case slang::TypeReflection::Kind::TextureBuffer:
						assert(false); // please implement me!
						break;
					case slang::TypeReflection::Kind::ShaderStorageBuffer:
						assert(false); // please implement me!
						break;
					case slang::TypeReflection::Kind::ParameterBlock:
						assert(false); // please implement me!
						break;
					case slang::TypeReflection::Kind::GenericTypeParameter:
						assert(false); // please implement me!
						break;
					case slang::TypeReflection::Kind::Interface:
						assert(false); // please implement me!
						break;
					default:
						assert(false); // please implement me!
						break;
					}

					bindings[parameter->getBindingSpace()].push_back(binding);
				}
			}

			std::cout << std::endl;

			unsigned fieldCount = typeLayout->getFieldCount();
			for (unsigned ff = 0; ff < fieldCount; ff++)
			{
				slang::VariableLayoutReflection* field = typeLayout->getFieldByIndex(ff);

				std::cout << "  name: " << field->getName()
							<< ", index: " << field->getBindingIndex()
							<< ", space: " << field->getBindingSpace(field->getCategory())
							<< ", offset: " << field->getOffset(field->getCategory())
							<< ", kind: " << (int)field->getType()->getKind()
							<< ", typeName: " << field->getType()->getName() << std::endl;
			}
		};

		slang::ShaderReflection* shaderReflection = slang::ShaderReflection::get(slangRequest);

		for (unsigned pp = 0; pp < shaderReflection->getParameterCount(); pp++)
			createLayoutBinding(shaderReflection->getParameterByIndex(pp));

		for (uint32_t epIndex = 0; epIndex < shaderReflection->getEntryPointCount(); epIndex++)
		{
			slang::EntryPointReflection* epReflection =
				shaderReflection->getEntryPointByIndex(epIndex);

			for (unsigned pp = 0; pp < epReflection->getParameterCount(); pp++)
				createLayoutBinding(epReflection->getParameterByIndex(pp));
		}

		spDestroyCompileRequest(slangRequest);
		spDestroySession(slangSession);
	};

	loadCachedSourceFile(
		slangFile, slangFile, "slang", "1.0.0-dev", loadSlang, loadPBin, savePBin);

	if (slangModule->shaders.empty())
		throw std::runtime_error("Failed to load shaders.");

	return slangModule;
}

template <GraphicsBackend B>
PipelineCacheHandle<B> loadPipelineCache(
	const std::filesystem::path& cacheFilePath,
	DeviceHandle<B> device,
	PhysicalDeviceProperties<B> physicalDeviceProperties)
{
	std::vector<std::byte> cacheData;

	auto loadCacheOp = [&physicalDeviceProperties, &cacheData](std::istream& stream)
	{
		cereal::BinaryInputArchive bin(stream);
		bin(cacheData);

		const PipelineCacheHeader<B>* header =
			reinterpret_cast<const PipelineCacheHeader<B>*>(cacheData.data());
		
		if (cacheData.empty() || !isCacheValid(*header, physicalDeviceProperties))
		{
			std::cout << "Invalid pipeline cache, creating new." << std::endl;
			cacheData.clear();
			return;
		}
	};

	FileInfo sourceFileInfo;
	if (getFileInfo(cacheFilePath, sourceFileInfo, false) != FileState::Missing)
		loadBinaryFile(cacheFilePath, sourceFileInfo, loadCacheOp, false);

	return createPipelineCache<B>(device, cacheData);
}

template <GraphicsBackend B>
void savePipelineCache(
	const std::filesystem::path& cacheFilePath,
	DeviceHandle<B> device,
	PhysicalDeviceProperties<B> physicalDeviceProperties,
	PipelineCacheHandle<B> pipelineCache)
{
	// todo: move to gfx-vulkan.cpp
	auto saveCacheOp = [&device, &pipelineCache, &physicalDeviceProperties](std::ostream& stream)
	{
		auto cacheData = getPipelineCacheData<B>(device, pipelineCache);
		if (!cacheData.empty())
		{
			const PipelineCacheHeader<B>* header =
				reinterpret_cast<const PipelineCacheHeader<B>*>(cacheData.data());

			if (cacheData.empty() || !isCacheValid(*header, physicalDeviceProperties))
			{
				std::cout << "Invalid pipeline cache, something is seriously wrong. Exiting." << std::endl;
				return;
			}
			
			cereal::BinaryOutputArchive bin(stream);
			bin(cacheData);
		}
		else
			assert(false && "Failed to get pipeline cache.");
	};

	FileInfo cacheFileInfo;
	saveBinaryFile(cacheFilePath, cacheFileInfo, saveCacheOp, false);
}
