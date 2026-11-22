#include <rhi/vulkan/pipelinebinaries.h>

#include <core/assert.h>
#include <core/file.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <format>
#include <random>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace rhi::pipeline
{

namespace binarystore
{

// a file: "SPB1", the driver's vendor id, device id and pipeline cache uuid, the binary count, then each binary's key
// size, key (VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR bytes), data size and data. little endian, as written
constexpr std::array<char, 4> kMagic{'S', 'P', 'B', '1'};

template <typename T>
void Put(std::vector<std::byte>& bytes, const T& value)
{
	const auto* begin = reinterpret_cast<const std::byte*>(&value);
	bytes.insert(bytes.end(), begin, begin + sizeof(T));
}

// reads from bytes (advanced), unless they're too few
template <typename T>
[[nodiscard]] bool Take(std::span<const std::byte>& bytes, T& value)
{
	if (bytes.size() < sizeof(T))
		return false;
	std::memcpy(&value, bytes.data(), sizeof(T));
	bytes = bytes.subspan(sizeof(T));
	return true;
}

std::string Hex(std::span<const uint8_t> bytes)
{
	std::string text;
	text.reserve(bytes.size() * 2);
	for (uint8_t byte : bytes)
		text += std::format("{:02x}", byte);
	return text;
}

// written beside path and renamed into place: another thread or process never reads half of it
void WriteReplacing(const std::filesystem::path& path, std::span<const std::byte> bytes)
{
	std::random_device random;
	auto temporary = path;
	temporary += std::format(".{:08x}", random());
	if (!core::file::Write(temporary, bytes))
		return;
	std::error_code error;
	std::filesystem::rename(temporary, path, error);
	if (error)
		std::filesystem::remove(temporary, error);
}

} // namespace binarystore

BinaryStore::BinaryStore(
	std::filesystem::path directory, VkDevice device, const VkAllocationCallbacks* allocator, const VkPhysicalDeviceProperties& properties)
	: myDirectory(std::move(directory))
	, myDevice(device)
	, myAllocator(allocator)
	, myVendorId(properties.vendorID)
	, myDeviceId(properties.deviceID)
{
	ENSUREF(
		gVkGetPipelineKeyKHR != nullptr && gVkCreatePipelineBinariesKHR != nullptr && gVkDestroyPipelineBinaryKHR != nullptr &&
			gVkGetPipelineBinaryDataKHR != nullptr && gVkReleaseCapturedPipelineDataKHR != nullptr,
		"pipeline binaries without VK_KHR_pipeline_binary's functions");
	std::memcpy(myUuid.data(), properties.pipelineCacheUUID, myUuid.size());
	std::error_code error;
	std::filesystem::create_directories(myDirectory, error);
}

VkPipeline BinaryStore::Create(VkGraphicsPipelineCreateInfo& info) const
{
	return InternalCreate(
		info,
		[this](const VkGraphicsPipelineCreateInfo& createInfo, VkPipeline& pipeline)
		{ return vkCreateGraphicsPipelines(myDevice, VK_NULL_HANDLE, 1, &createInfo, myAllocator, &pipeline); });
}

VkPipeline BinaryStore::Create(VkComputePipelineCreateInfo& info) const
{
	return InternalCreate(
		info,
		[this](const VkComputePipelineCreateInfo& createInfo, VkPipeline& pipeline)
		{ return vkCreateComputePipelines(myDevice, VK_NULL_HANDLE, 1, &createInfo, myAllocator, &pipeline); });
}

template <typename CreateInfo, typename CreateFn>
VkPipeline BinaryStore::InternalCreate(CreateInfo& info, CreateFn create) const
{
	using namespace binarystore;

	// the pipeline's key: what its create info makes, the file's name
	VkPipelineCreateInfoKHR keyInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_INFO_KHR, .pNext = &info};
	VkPipelineBinaryKeyKHR key{.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
	VK_CHECK(gVkGetPipelineKeyKHR(myDevice, &keyInfo, &key));
	const auto path = myDirectory / std::format("{}.bin", Hex(std::span(key.key).first(std::min<uint32_t>(key.keySize, VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR))));
	const void* next = info.pNext;
	VkPipeline pipeline = VK_NULL_HANDLE;

	// made from its binaries, where the file has them for this driver (they're the driver's to refuse too: made again)
	if (auto file = core::file::Map(path); file && file->size() > 0)
	{
		std::span<const std::byte> bytes(file->data(), file->size());
		std::array<char, 4> magic{};
		uint32_t vendorId = 0;
		uint32_t deviceId = 0;
		std::array<uint8_t, VK_UUID_SIZE> uuid{};
		uint32_t binaryCount = 0;
		bool valid = Take(bytes, magic) && Take(bytes, vendorId) && Take(bytes, deviceId) && Take(bytes, uuid) && Take(bytes, binaryCount) &&
					 magic == kMagic && vendorId == myVendorId && deviceId == myDeviceId && uuid == myUuid && binaryCount > 0;
		std::vector<VkPipelineBinaryKeyKHR> keys;
		std::vector<VkPipelineBinaryDataKHR> data;
		for (uint32_t binaryIt = 0; valid && binaryIt < binaryCount; binaryIt++)
		{
			auto& binaryKey = keys.emplace_back(VkPipelineBinaryKeyKHR{.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR});
			uint64_t dataSize = 0;
			valid = Take(bytes, binaryKey.keySize) && Take(bytes, binaryKey.key) && Take(bytes, dataSize) &&
					binaryKey.keySize <= VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR && dataSize <= bytes.size();
			if (valid)
			{
				data.push_back({.dataSize = static_cast<size_t>(dataSize), .pData = const_cast<std::byte*>(bytes.data())});
				bytes = bytes.subspan(static_cast<size_t>(dataSize));
			}
		}
		if (valid)
		{
			VkPipelineBinaryKeysAndDataKHR keysAndData{.binaryCount = binaryCount, .pPipelineBinaryKeys = keys.data(), .pPipelineBinaryData = data.data()};
			VkPipelineBinaryCreateInfoKHR binaryInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR, .pKeysAndDataInfo = &keysAndData};
			std::vector<VkPipelineBinaryKHR> binaries(binaryCount);
			VkPipelineBinaryHandlesInfoKHR handles{
				.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR, .pipelineBinaryCount = binaryCount, .pPipelineBinaries = binaries.data()};
			if (gVkCreatePipelineBinariesKHR(myDevice, &binaryInfo, myAllocator, &handles) == VK_SUCCESS)
			{
				VkPipelineBinaryInfoKHR fromBinaries{
					.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR, .pNext = next, .binaryCount = binaryCount, .pPipelineBinaries = binaries.data()};
				info.pNext = &fromBinaries;
				VkResult result = create(info, pipeline);
				info.pNext = next;
				for (auto binary : binaries)
					gVkDestroyPipelineBinaryKHR(myDevice, binary, myAllocator);
				if (result == VK_SUCCESS)
				{
					// (its time: its last use, which Evict goes by)
					std::error_code error;
					std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now(), error);
					return pipeline;
				}
				pipeline = VK_NULL_HANDLE;
			}
		}
	}

	// compiled (the driver's own cache may hold it), its data captured for its binaries
	VkPipelineCreateFlags2CreateInfoKHR flags{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR,
		.pNext = next,
		.flags = static_cast<VkPipelineCreateFlags2KHR>(info.flags) | VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR};
	info.pNext = &flags;
	VK_CHECK(create(info, pipeline));
	info.pNext = next;

	// its binaries, kept: each one's key and data
	VkPipelineBinaryCreateInfoKHR fromPipeline{.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR, .pipeline = pipeline};
	VkPipelineBinaryHandlesInfoKHR handles{.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR};
	if (gVkCreatePipelineBinariesKHR(myDevice, &fromPipeline, myAllocator, &handles) == VK_SUCCESS && handles.pipelineBinaryCount > 0)
	{
		std::vector<VkPipelineBinaryKHR> binaries(handles.pipelineBinaryCount, VK_NULL_HANDLE);
		handles.pPipelineBinaries = binaries.data();
		if (gVkCreatePipelineBinariesKHR(myDevice, &fromPipeline, myAllocator, &handles) == VK_SUCCESS)
		{
			std::vector<std::byte> bytes;
			Put(bytes, kMagic);
			Put(bytes, myVendorId);
			Put(bytes, myDeviceId);
			Put(bytes, myUuid);
			Put(bytes, handles.pipelineBinaryCount);
			bool complete = true;
			std::vector<std::byte> data;
			for (auto binary : binaries)
			{
				VkPipelineBinaryDataInfoKHR dataInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_DATA_INFO_KHR, .pipelineBinary = binary};
				VkPipelineBinaryKeyKHR binaryKey{.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
				size_t dataSize = 0;
				complete = complete && gVkGetPipelineBinaryDataKHR(myDevice, &dataInfo, &binaryKey, &dataSize, nullptr) == VK_SUCCESS;
				data.resize(dataSize);
				complete = complete && gVkGetPipelineBinaryDataKHR(myDevice, &dataInfo, &binaryKey, &dataSize, data.data()) == VK_SUCCESS;
				Put(bytes, binaryKey.keySize);
				Put(bytes, binaryKey.key);
				Put(bytes, static_cast<uint64_t>(dataSize));
				bytes.insert(bytes.end(), data.begin(), data.begin() + static_cast<ptrdiff_t>(std::min(dataSize, data.size())));
			}
			if (complete)
				WriteReplacing(path, bytes);
		}
		for (auto binary : binaries)
			if (binary != VK_NULL_HANDLE)
				gVkDestroyPipelineBinaryKHR(myDevice, binary, myAllocator);
	}
	VkReleaseCapturedPipelineDataInfoKHR release{.sType = VK_STRUCTURE_TYPE_RELEASE_CAPTURED_PIPELINE_DATA_INFO_KHR, .pipeline = pipeline};
	VK_CHECK(gVkReleaseCapturedPipelineDataKHR(myDevice, &release, myAllocator));
	return pipeline;
}

uint64_t BinaryStore::Evict(uint64_t byteCount) const
{
	struct Entry
	{
		std::filesystem::path path;
		uint64_t size = 0;
		std::filesystem::file_time_type time;
	};
	std::vector<Entry> entries;
	uint64_t total = 0;
	std::error_code error;
	const auto now = std::filesystem::file_time_type::clock::now();
	for (auto it = std::filesystem::directory_iterator(myDirectory, error); !error && it != std::filesystem::directory_iterator(); it.increment(error))
	{
		std::error_code entryError;
		if (!it->is_regular_file(entryError))
			continue;
		auto size = it->file_size(entryError);
		auto time = it->last_write_time(entryError);
		if (entryError)
			continue;
		// a write that didn't finish (one in flight, another thread's or process's, is younger)
		if (it->path().extension() != ".bin")
		{
			if (now - time > std::chrono::minutes(10))
				std::filesystem::remove(it->path(), entryError);
			continue;
		}
		entries.push_back({.path = it->path(), .size = size, .time = time});
		total += size;
	}
	if (total <= byteCount)
		return total;
	std::ranges::sort(entries, {}, &Entry::time);
	for (const auto& entry : entries)
	{
		if (total <= byteCount / 4 * 3)
			break;
		if (std::filesystem::remove(entry.path, error))
			total -= entry.size;
	}
	return total;
}

} // namespace rhi::pipeline
