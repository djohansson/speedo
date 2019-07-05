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
std::shared_ptr<SerializableShaderReflectionModule<B>> loadSlangShaders(const std::filesystem::path& slangFile)
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
	DeviceHandle<B> device,
	PhysicalDeviceProperties<B> physicalDeviceProperties,
	const std::filesystem::path& cacheFilePath)
{
	std::vector<std::byte> cacheData;

	auto loadCache = [&physicalDeviceProperties, &cacheData](std::istream& stream) {
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
		loadBinaryFile(cacheFilePath, sourceFileInfo, loadCache, false);

	auto createPipelineCache = [](DeviceHandle<B> device, const std::vector<std::byte>& cacheData)
	{
		PipelineCacheHandle<B> cache;

		VkPipelineCacheCreateInfo createInfo = {};
		createInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
		createInfo.flags = 0;
		createInfo.initialDataSize = cacheData.size();
		createInfo.pInitialData = cacheData.size() ? cacheData.data() : nullptr;
	
		CHECK_VK(vkCreatePipelineCache(device, &createInfo, nullptr, &cache));
	
		return cache;
	};

	return createPipelineCache(device, cacheData);
}

template <GraphicsBackend B>
void savePipelineCache(
	DeviceHandle<B> device,
	PipelineCacheHandle<B> pipelineCache,
	PhysicalDeviceProperties<B> physicalDeviceProperties,
	const std::filesystem::path& cacheFilePath)
{
	auto savePipelineCacheData = [&device, &pipelineCache, &physicalDeviceProperties](std::ostream& stream) {
		size_t cacheDataSize = 0;
		CHECK_VK(vkGetPipelineCacheData(device, pipelineCache, &cacheDataSize, nullptr));
		if (cacheDataSize)
		{
			std::vector<std::byte> cacheData(cacheDataSize);
			CHECK_VK(vkGetPipelineCacheData(device, pipelineCache, &cacheDataSize, cacheData.data()));

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
	saveBinaryFile(cacheFilePath, cacheFileInfo, savePipelineCacheData, false);
}

template <GraphicsBackend B>
InstanceHandle<B> createInstance()
{
    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "No Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_1;

    uint32_t instanceLayerCount;
    CHECK_VK(vkEnumerateInstanceLayerProperties(&instanceLayerCount, nullptr));
    std::cout << instanceLayerCount << " layers found!\n";
    if (instanceLayerCount > 0)
    {
        std::unique_ptr<VkLayerProperties[]> instanceLayers(
            new VkLayerProperties[instanceLayerCount]);
        CHECK_VK(vkEnumerateInstanceLayerProperties(&instanceLayerCount, instanceLayers.get()));
        for (uint32_t i = 0; i < instanceLayerCount; ++i)
            std::cout << instanceLayers[i].layerName << "\n";
    }

    const char* enabledLayerNames[] =
    {
    #ifdef PROFILING_ENABLED
        "VK_LAYER_LUNARG_standard_validation",
    #endif
    };

    uint32_t instanceExtensionCount;
    vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount, nullptr);

    std::vector<VkExtensionProperties> availableInstanceExtensions(instanceExtensionCount);
    vkEnumerateInstanceExtensionProperties(
        nullptr, &instanceExtensionCount, availableInstanceExtensions.data());

    std::vector<const char*> instanceExtensions(instanceExtensionCount);
    for (uint32_t i = 0; i < instanceExtensionCount; i++)
    {
        instanceExtensions[i] = availableInstanceExtensions[i].extensionName;
        std::cout << instanceExtensions[i] << "\n";
    }

    std::sort(
        instanceExtensions.begin(), instanceExtensions.end(),
        [](const char* lhs, const char* rhs) { return strcmp(lhs, rhs) < 0; });

    std::vector<const char*> requiredExtensions = {
        // must be sorted lexicographically for std::includes to work!
        "VK_EXT_debug_report",
        "VK_KHR_surface",
#if defined(__WINDOWS__)
        "VK_KHR_win32_surface",
#elif defined(__APPLE__)
        "VK_MVK_macos_surface",
#elif defined(__linux__)
#	if defined(VK_USE_PLATFORM_XCB_KHR)
        "VK_KHR_xcb_surface",
#	elif defined(VK_USE_PLATFORM_XLIB_KHR)
        "VK_KHR_xlib_surface",
#	elif defined(VK_USE_PLATFORM_WAYLAND_KHR)
        "VK_KHR_wayland_surface",
#	else // default to xcb
        "VK_KHR_xcb_surface",
#	endif
#endif
    };

    assert(std::includes(
        instanceExtensions.begin(), instanceExtensions.end(), requiredExtensions.begin(),
        requiredExtensions.end(),
        [](const char* lhs, const char* rhs) { return strcmp(lhs, rhs) < 0; }));

    // if (std::find(instanceExtensions.begin(), instanceExtensions.end(), "VK_KHR_display") ==
    // instanceExtensions.end()) 	instanceExtensions.push_back("VK_KHR_display");

    VkInstanceCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &appInfo;
    info.enabledLayerCount = sizeof_array(enabledLayerNames);
    info.ppEnabledLayerNames = info.enabledLayerCount ? enabledLayerNames : nullptr;
    info.enabledExtensionCount = static_cast<uint32_t>(requiredExtensions.size());
    info.ppEnabledExtensionNames = requiredExtensions.data();

    InstanceHandle<B> instance = 0;
    CHECK_VK(vkCreateInstance(&info, NULL, &instance));

    return instance;
}
