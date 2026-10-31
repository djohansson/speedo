#include <rhi/vulkan/pipelinecache.h>

#include <rhi/rhi.h>
#include <rhi/vulkan/utils.h>

#include <core/file.h>

#include <filesystem>

namespace rhi
{

#pragma pack(push, 1)
template <>
struct PipelineCacheHeader<kVk>
{
	uint32_t headerLength = 0UL;
	uint32_t cacheHeaderVersion = 0UL;
	uint32_t vendorID = 0UL;
	uint32_t deviceID = 0UL;
	std::array<uint8_t, VK_UUID_SIZE> pipelineCacheUUID;
};
#pragma pack(pop)

namespace pipeline
{

using namespace core::file;

static bool IsCacheValid(
	const PipelineCacheHeader<kVk>& header,
	const PhysicalDeviceProperties<kVk>& physicalDeviceProperties)
{
	return (
		header.headerLength > 0 &&
		header.cacheHeaderVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
		header.vendorID == physicalDeviceProperties.properties.vendorID &&
		header.deviceID == physicalDeviceProperties.properties.deviceID &&
		memcmp(
			header.pipelineCacheUUID.data(),
			physicalDeviceProperties.properties.pipelineCacheUUID,
			std::size(header.pipelineCacheUUID)) == 0);
}

PipelineCacheHandle<kVk> LoadPipelineCache(const std::filesystem::path& cacheFilePath, Device<kVk>& device, const Instance<kVk>& instance)
{
	std::vector<char> cacheData;

	auto loadCacheOp = [&instance, &device, &cacheData](auto& inStream) -> std::error_code
	{
		if (auto result = inStream(cacheData); failure(result))
			return std::make_error_code(result);

		const auto* header = reinterpret_cast<const PipelineCacheHeader<kVk>*>(cacheData.data());

		const auto& properties = instance.GetPhysicalDeviceInfo(device.GetPhysicalDevice()).deviceProperties.properties;
		if (cacheData.empty() ||
			!IsCacheValid(*header, instance.GetPhysicalDeviceInfo(device.GetPhysicalDevice()).deviceProperties))
		{
			if (cacheData.size() < sizeof(PipelineCacheHeader<kVk>))
				std::println(stderr, "Invalid pipeline cache ({} bytes), creating new.", cacheData.size());
			else
				std::println(
					stderr,
					"Invalid pipeline cache (header {} bytes, version {}, vendor {:#x} against {:#x}, device {:#x} against {:#x}, uuid {}), creating new.",
					header->headerLength,
					header->cacheHeaderVersion,
					header->vendorID,
					properties.vendorID,
					header->deviceID,
					properties.deviceID,
					memcmp(header->pipelineCacheUUID.data(), properties.pipelineCacheUUID, VK_UUID_SIZE) == 0 ? "the same" : "another");
			cacheData.clear();
		}

		return {};
	};

	if (auto fileInfo = GetRecord<false>(cacheFilePath); fileInfo)
		fileInfo = LoadBinary<false>(cacheFilePath, loadCacheOp);

	VkPipelineCacheCreateInfo createInfo{.sType=VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
	createInfo.initialDataSize = cacheData.size();
	createInfo.pInitialData =
		(static_cast<unsigned int>(!cacheData.empty()) != 0U) ? cacheData.data() : nullptr;

	PipelineCacheHandle<kVk> cache;
	VK_CHECK(vkCreatePipelineCache(
		device,
		&createInfo,
		&instance.GetHostAllocationCallbacks(),
		&cache));

	return cache;
}

static std::vector<std::byte>
GetPipelineCacheData(DeviceHandle<kVk> device, PipelineCacheHandle<kVk> pipelineCache)
{
	std::vector<std::byte> cacheData;
	size_t cacheDataSize = 0;
	VK_CHECK(vkGetPipelineCacheData(device, pipelineCache, &cacheDataSize, nullptr));
	if (cacheDataSize != 0U)
	{
		cacheData.resize(cacheDataSize);
		VK_CHECK(vkGetPipelineCacheData(device, pipelineCache, &cacheDataSize, cacheData.data()));
	}

	return cacheData;
}

std::expected<Record, std::error_code> SavePipelineCache(
	const std::filesystem::path& cacheFilePath,
	DeviceHandle<kVk> device,
	PhysicalDeviceProperties<kVk> physicalDeviceProperties,
	PipelineCacheHandle<kVk> pipelineCache)
{
	// todo: move to gfx-vulkan.cpp
	auto saveCacheOp =
		[&device, &pipelineCache, &physicalDeviceProperties](auto& out) -> std::error_code
	{
		auto cacheData = GetPipelineCacheData(device, pipelineCache);

		if (cacheData.empty())
		{
			std::println("Failed to get pipeline cache.");
		
			return std::make_error_code(std::errc::invalid_argument);
		}

		const auto* header = reinterpret_cast<const PipelineCacheHeader<kVk>*>(cacheData.data());

		if (!IsCacheValid(*header, physicalDeviceProperties))
		{
			std::println("Invalid pipeline cache, will not save.");

			return std::make_error_code(std::errc::invalid_argument);
		}

		if (auto result = out(cacheData); failure(result))
			return std::make_error_code(result);

		return {}; // success
	};

	return SaveBinary<true>(cacheFilePath, saveCacheOp);
}

} // namespace pipeline

} // namespace rhi
