#pragma once

#include <rhi/vulkan/utils.h>

#include <array>
#include <cstdint>
#include <filesystem>

// a pipeline's binaries kept on disk (VK_KHR_pipeline_binary), in place of a pipeline cache's file where the device has
// them (see Pipeline): a VkPipelineCache's data is all or nothing, so a profile's only grew (every pipeline once made
// stayed, 240 to 460 MB in the GI experiments' profile slots), and loading and saving it cost every run 1.3 s at 458 MB.
// here each pipeline's binaries are a file of their own in a directory, named by the pipeline's key (vkGetPipelineKeyKHR,
// what its create info makes), and a file's time is when a pipeline was last made from it, so that the least recently
// used go once the directory holds more than kPipelineCacheByteCount (Evict). the backend's own (not part of rhi's api)
namespace rhi::pipeline
{

class BinaryStore final
{
public:
	// the device's (which enables VK_KHR_pipeline_binary) binaries in directory, made if it isn't there; properties name
	// the driver whose binaries they are (a file of another's is left out, and made again)
	BinaryStore(
		std::filesystem::path directory, VkDevice device, const VkAllocationCallbacks* allocator, const VkPhysicalDeviceProperties& properties);

	// a pipeline of info (whose pNext the call extends, and restores): made from its binaries where the directory has
	// them, else compiled (the driver's own cache may have it), its binaries captured and kept. any thread
	[[nodiscard]] VkPipeline Create(VkGraphicsPipelineCreateInfo& info) const;
	[[nodiscard]] VkPipeline Create(VkComputePipelineCreateInfo& info) const;

	// the least recently used files removed while the directory holds more than byteCount, to three quarters of it
	// (so that the next runs' new ones don't each evict again); returns how many bytes it holds
	uint64_t Evict(uint64_t byteCount) const;

	[[nodiscard]] const std::filesystem::path& GetDirectory() const noexcept { return myDirectory; }

private:
	template <typename CreateInfo, typename CreateFn>
	[[nodiscard]] VkPipeline InternalCreate(CreateInfo& info, CreateFn create) const;

	std::filesystem::path myDirectory;
	VkDevice myDevice{};
	const VkAllocationCallbacks* myAllocator = nullptr;
	// the driver the binaries are of: its vendor, device and pipeline cache's uuid
	uint32_t myVendorId = 0;
	uint32_t myDeviceId = 0;
	std::array<uint8_t, VK_UUID_SIZE> myUuid{};
};

} // namespace rhi::pipeline
