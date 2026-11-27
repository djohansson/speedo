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

std::filesystem::path VersionedPipelineCachePath(const std::filesystem::path& cacheFilePath, std::string_view version)
{
	auto path = cacheFilePath;
	path.replace_filename(std::format("{}.{}{}", cacheFilePath.stem().string(), version, cacheFilePath.extension().string()));
	return path;
}

uint64_t EvictPipelineCaches(const std::filesystem::path& cacheFilePath, uint64_t byteCount, const std::filesystem::path& keep)
{
	// the versions' files: <stem>.<16 hex digits><extension>
	constexpr size_t kVersionLength = 16;
	auto stem = cacheFilePath.stem().string() + ".";
	auto extension = cacheFilePath.extension().string();
	struct Entry
	{
		std::filesystem::path path;
		std::filesystem::file_time_type written;
		uint64_t size = 0;
	};
	std::vector<Entry> entries;
	std::error_code error;
	for (const auto& file : std::filesystem::directory_iterator(cacheFilePath.parent_path(), error))
	{
		auto name = file.path().filename().string();
		if (name.size() != stem.size() + kVersionLength + extension.size() || !name.starts_with(stem) || !name.ends_with(extension))
			continue;
		auto version = std::string_view(name).substr(stem.size(), kVersionLength);
		if (!std::ranges::all_of(version, [](char digit) { return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f'); }))
			continue;
		std::error_code fileError;
		Entry entry{.path = file.path(), .written = file.last_write_time(fileError)};
		if (fileError)
			continue;
		entry.size = file.file_size(fileError);
		if (!fileError)
			entries.push_back(std::move(entry));
	}
	// the latest first, keep before all
	std::ranges::sort(entries, [&](const Entry& lhs, const Entry& rhs) {
		bool lhsKept = lhs.path == keep;
		bool rhsKept = rhs.path == keep;
		return lhsKept != rhsKept ? lhsKept : lhs.written > rhs.written;
	});
	uint64_t kept = 0;
	for (const auto& entry : entries)
	{
		if (entry.path == keep || kept + entry.size <= byteCount)
		{
			kept += entry.size;
			continue;
		}
		if (std::filesystem::remove(entry.path, error))
			std::println("Removed the pipeline cache {} ({} MB): its version's the least recently used past {} MB", entry.path.string(), entry.size >> 20U, byteCount >> 20U);
		else
			kept += entry.size;
	}
	return kept;
}

size_t PipelineCacheByteCount(DeviceHandle<kVk> device, PipelineCacheHandle<kVk> pipelineCache)
{
	size_t byteCount = 0;
	VK_CHECK(vkGetPipelineCacheData(device, pipelineCache, &byteCount, nullptr));
	return byteCount;
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

	// one grown past kPipelineCacheByteCount started over: its data is all or nothing (a pipeline once made stays in it),
	// and every run loaded and saved all of it (a device without pipeline binaries, see rhi/vulkan/pipelinebinaries.h).
	// this run's pipelines are saved, made again or from the driver's own cache
	std::error_code error;
	if (auto size = std::filesystem::file_size(cacheFilePath, error); !error && size > kPipelineCacheByteCount)
		std::println("The pipeline cache {} is {} MB, over {}: started over", cacheFilePath.string(), size >> 20U, kPipelineCacheByteCount >> 20U);
	else if (auto fileInfo = GetRecord<false>(cacheFilePath); fileInfo)
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

	// (no checksum of what was written: nothing reads it, and its SHA-256 read the whole file again, 0.7 s of a run's exit
	// at 458 MB)
	return SaveBinary<false>(cacheFilePath, saveCacheOp);
}

} // namespace pipeline

} // namespace rhi
