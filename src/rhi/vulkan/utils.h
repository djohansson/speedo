#pragma once

#include <rhi/deviceobject.h>
#include <rhi/types.h>
#include <rhi/vulkan/convert.h>

#include <span>
#include <string_view>
#include <type_traits>

// checked in every build, release included: an error (a lost device, out of memory, ...) writes a crash report and ends
// the process (see crashreport.h); another unexpected result (a timeout, suboptimal, ...) is logged, and traps in builds
// that profile. the optional argument is the queue the call was on (its checkpoints, in the report)
void OnCheckFailedDefault(VkResult result, uintptr_t count, ...);
#define VK_CHECK_RESULT_IMPL(Expression, ExpectedResult, FailCallback, ...) do \
{ \
	VkResult __result = (Expression); \
	if (__result != ExpectedResult) [[unlikely]] \
		FailCallback(__result, SIZEOF__VA_ARGS__(__VA_ARGS__) __VA_OPT__(,) __VA_ARGS__); \
} while(false)
#define VK_CHECK_RESULT(Expression, ExpectedResult, ...) \
	VK_CHECK_RESULT_IMPL(Expression, ExpectedResult, OnCheckFailedDefault __VA_OPT__(,) __VA_ARGS__)
#define VK_CHECK(Expression, ...) \
	VK_CHECK_RESULT_IMPL(Expression, VK_SUCCESS, OnCheckFailedDefault __VA_OPT__(,) __VA_ARGS__)

// the vulkan version the instance asks for and the devices must support: what it made core (dynamic rendering,
// synchronization2, the dynamic cull mode and front face, inline uniform blocks, push descriptors, ...) is called and
// enabled as core, rather than through extensions
inline constexpr uint32_t kVulkanApiVersion = VK_API_VERSION_1_4;

// the extensions' functions (what isn't core in kVulkanApiVersion)
extern PFN_vkWaitForPresentKHR gVkWaitForPresentKHR;
#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
extern PFN_vkCreateDebugUtilsMessengerEXT gVkCreateDebugUtilsMessengerEXT;
extern PFN_vkDestroyDebugUtilsMessengerEXT gVkDestroyDebugUtilsMessengerEXT;
extern PFN_vkSetDebugUtilsObjectNameEXT gVkSetDebugUtilsObjectNameExt;
#endif
extern PFN_vkCmdSetCheckpointNV gVkCmdSetCheckpointNV;
extern PFN_vkGetQueueCheckpointData2NV gVkGetQueueCheckpointData2NV;
// ray queries' acceleration structures (null unless the device enables them, see WantsRayQueries)
extern PFN_vkCreateAccelerationStructureKHR gVkCreateAccelerationStructureKHR;
extern PFN_vkDestroyAccelerationStructureKHR gVkDestroyAccelerationStructureKHR;
extern PFN_vkGetAccelerationStructureBuildSizesKHR gVkGetAccelerationStructureBuildSizesKHR;
extern PFN_vkGetAccelerationStructureDeviceAddressKHR gVkGetAccelerationStructureDeviceAddressKHR;
extern PFN_vkCmdBuildAccelerationStructuresKHR gVkCmdBuildAccelerationStructuresKHR;
extern PFN_vkCmdWriteAccelerationStructuresPropertiesKHR gVkCmdWriteAccelerationStructuresPropertiesKHR;
extern PFN_vkCmdCopyAccelerationStructureKHR gVkCmdCopyAccelerationStructureKHR;

// tracks (and names) a vulkan object, or stops tracking it, see rhi::TrackObject/TrackInstanceObject
template <typename HandleT>
[[nodiscard]] constexpr uint64_t ToObjectHandle(HandleT handle) noexcept
{
	if constexpr (std::is_pointer_v<HandleT>)
		return reinterpret_cast<uint64_t>(handle);
	else
		return static_cast<uint64_t>(handle);
}
template <typename HandleT>
void Track(VkDevice device, VkObjectType type, HandleT handle, std::string_view name)
{
	rhi::TrackObject<kVk>(device, type, ToObjectHandle(handle), name);
}
template <typename HandleT>
void TrackInstance(VkObjectType type, HandleT handle, std::string_view name)
{
	rhi::TrackInstanceObject<kVk>(type, ToObjectHandle(handle), name);
}
template <typename HandleT>
void Untrack(VkObjectType type, HandleT handle)
{
	rhi::UntrackObject<kVk>(type, ToObjectHandle(handle));
}

void InitInstanceExtensions(VkInstance instance);
void InitDeviceExtensions(VkDevice device);

[[nodiscard]] bool SupportsExtension(const char* extensionName, VkInstance device);
[[nodiscard]] bool SupportsExtension(const char* extensionName, VkPhysicalDevice device);
// whether a device is made with ray queries (VK_KHR_ray_query, its acceleration structures and their deferred host
// operations, see DeviceLimits::rayQuery): where it has the extensions, unless SPEEDO_RAY_QUERY=0
[[nodiscard]] bool WantsRayQueries(VkPhysicalDevice device);

template <typename T>
[[nodiscard]] extern bool SupportsFeature(const T& feature);

[[nodiscard]] uint32_t GetFormatSize(VkFormat format, uint32_t& outDivisor);
[[nodiscard]] uint32_t GetFormatSize(VkFormat format);

[[nodiscard]] bool HasColorComponent(VkFormat format);
[[nodiscard]] bool HasStencilComponent(VkFormat format);
[[nodiscard]] bool HasDepthComponent(VkFormat format);

[[nodiscard]] inline bool HasColorComponent(rhi::Format format) { return HasColorComponent(rhi::vk::ToVk(format)); }
[[nodiscard]] inline bool HasStencilComponent(rhi::Format format) { return HasStencilComponent(rhi::vk::ToVk(format)); }
[[nodiscard]] inline bool HasDepthComponent(rhi::Format format) { return HasDepthComponent(rhi::vk::ToVk(format)); }

// the aspects of an image of format: color, or depth and/or stencil
[[nodiscard]] inline rhi::ImageAspect AspectOf(rhi::Format format)
{
	if (HasColorComponent(format))
		return rhi::ImageAspect::kColor;
	auto aspect = rhi::ImageAspect::kNone;
	if (HasDepthComponent(format))
		aspect |= rhi::ImageAspect::kDepth;
	if (HasStencilComponent(format))
		aspect |= rhi::ImageAspect::kStencil;
	return aspect;
}

[[nodiscard]] uint32_t
FindMemoryType(VkPhysicalDevice device, uint32_t typeFilter, VkMemoryPropertyFlags properties);

[[nodiscard]] VkFormat
FindSupportedFormat(
	VkPhysicalDevice device,
	std::span<const VkFormat> candidates,
	VkImageTiling tiling,
	VkFormatFeatureFlags features);

void CopyBuffer(
	VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);

[[nodiscard]] std::tuple<VkBuffer, VmaAllocation> CreateBuffer(
	VmaAllocator allocator,
	VkDeviceSize size,
	VkBufferUsageFlags usage,
	VkMemoryPropertyFlags flags,
	const char* debugName);

[[nodiscard]] std::tuple<VkBuffer, VmaAllocation> CreateBuffer(
	VkCommandBuffer commandBuffer,
	VmaAllocator allocator,
	VkBuffer stagingBuffer,
	VkDeviceSize bufferSize,
	VkBufferUsageFlags usage,
	VkMemoryPropertyFlags memoryFlags,
	const char* debugName);

// a buffer or 2d image placed at offset in allocation (which they don't own: Destroy* them with a null allocation), and
// what such an image or buffer needs of the memory, from a temporary one
[[nodiscard]] VkBuffer CreateAliasingBuffer(
	VmaAllocator allocator, VmaAllocation allocation, VkDeviceSize offset, VkDeviceSize size, VkBufferUsageFlags usage, const char* debugName);
[[nodiscard]] VkMemoryRequirements GetBufferMemoryRequirements(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage);
[[nodiscard]] VkImageCreateInfo ImageCreateInfo2D(
	uint32_t width, uint32_t height, uint32_t mipLevels, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage, VkImageLayout initialLayout);
[[nodiscard]] VkImage CreateAliasingImage(
	VmaAllocator allocator, VmaAllocation allocation, VkDeviceSize offset, const VkImageCreateInfo& info, const char* debugName);
[[nodiscard]] VkMemoryRequirements GetImageMemoryRequirements(VmaAllocator allocator, const VkImageCreateInfo& info);

// destroy what the Create* functions in this file created (they track the objects, see Track)
void DestroyBuffer(VmaAllocator allocator, VkBuffer buffer, VmaAllocation memory);
void DestroyImage(VmaAllocator allocator, VkImage image, VmaAllocation memory);
void DestroyImageView(VkDevice device, const VkAllocationCallbacks* hostAllocationCallbacks, VkImageView imageView);
void DestroyFramebuffer(VkDevice device, const VkAllocationCallbacks* hostAllocator, VkFramebuffer framebuffer);
void DestroyRenderPass(VkDevice device, const VkAllocationCallbacks* hostAllocator, VkRenderPass renderPass);

[[nodiscard]] std::tuple<VkBuffer, VmaAllocation> CreateStagingBuffer(
	VmaAllocator allocator,
	const void* srcData,
	size_t srcDataSize,
	const char* debugName);

void TransitionImageLayout(
	VkCommandBuffer commandBuffer,
	VkImage image,
	VkFormat format,
	VkImageLayout oldLayout,
	VkImageLayout newLayout,
	uint32_t mipLevels,
	VkImageAspectFlags aspectFlags);
// the same for levelCount mip levels from baseMipLevel
void TransitionImageLayout(
	VkCommandBuffer commandBuffer,
	VkImage image,
	VkImageLayout oldLayout,
	VkImageLayout newLayout,
	uint32_t baseMipLevel,
	uint32_t levelCount,
	VkImageAspectFlags aspectFlags);

void CopyBufferToImage(
	VkCommandBuffer commandBuffer,
	VkBuffer buffer,
	VkImage image,
	uint32_t width,
	uint32_t height,
	uint32_t mipLevels,
	const uint32_t* mipOffsets,
	uint32_t mipOffsetsStride);

[[nodiscard]] std::tuple<VkImage, VmaAllocation> CreateImage2D(
	VmaAllocator allocator,
	uint32_t width,
	uint32_t height,
	uint32_t mipLevels,
	VkFormat format,
	VkImageTiling tiling,
	VkImageUsageFlags usage,
	VkMemoryPropertyFlags memoryFlags,
	const char* debugName,
	VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED);

// a volume (VK_IMAGE_TYPE_3D), as CreateImage2D with a depth
[[nodiscard]] std::tuple<VkImage, VmaAllocation> CreateImage3D(
	VmaAllocator allocator,
	uint32_t width,
	uint32_t height,
	uint32_t depth,
	uint32_t mipLevels,
	VkFormat format,
	VkImageTiling tiling,
	VkImageUsageFlags usage,
	VkMemoryPropertyFlags memoryFlags,
	const char* debugName,
	VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED);

[[nodiscard]] std::tuple<VkImage, VmaAllocation> CreateImage2D(
	VkCommandBuffer commandBuffer,
	VmaAllocator allocator,
	VkBuffer stagingBuffer,
	uint32_t width,
	uint32_t height,
	uint32_t mipLevels,
	const uint32_t* mipOffsets,
	uint32_t mipOffsetsStride,
	VkFormat format,
	VkImageTiling tiling,
	VkImageUsageFlags usage,
	VkMemoryPropertyFlags memoryFlags,
	VkImageAspectFlags aspectFlags,
	const char* debugName,
	VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED);

[[nodiscard]] VkImageView CreateImageView2D(
	VkDevice device,
	const VkAllocationCallbacks* hostAllocationCallbacks,
	VkImageViewCreateFlags flags,
	VkImage image,
	VkFormat format,
	VkImageAspectFlags aspectFlags,
	uint32_t mipLevels,
	std::string_view debugName = {},
	VkComponentMapping components = {},
	uint32_t baseMipLevel = 0,
	VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D);

[[nodiscard]] VkFramebuffer CreateFramebuffer(
	VkDevice device,
	const VkAllocationCallbacks* hostAllocator,
	VkRenderPass renderPass,
	uint32_t attachmentCount,
	const VkImageView* attachments,
	uint32_t width,
	uint32_t height,
	uint32_t layers,
	std::string_view debugName = {});

[[nodiscard]] VkRenderPass CreateRenderPass(
	VkDevice device,
	const VkAllocationCallbacks* hostAllocator,
	std::span<const VkAttachmentDescription2> attachments,
	std::span<const VkSubpassDescription2> subpasses,
	std::span<const VkSubpassDependency2> subpassDependencies,
	std::string_view debugName = {});

[[nodiscard]] VkRenderPass CreateRenderPass(
	VkDevice device,
	const VkAllocationCallbacks* hostAllocator,
	VkPipelineBindPoint bindPoint,
	VkFormat colorFormat,
	VkAttachmentLoadOp colorLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
	VkAttachmentStoreOp colorStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
	VkImageLayout colorInitialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	VkImageLayout colorFinalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
	VkFormat depthFormat = VK_FORMAT_UNDEFINED,
	VkAttachmentLoadOp depthLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
	VkAttachmentStoreOp depthStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
	VkImageLayout depthInitialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	VkImageLayout depthFinalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

[[nodiscard]] VkSurfaceKHR CreateSurface(VkInstance instance, const VkAllocationCallbacks* hostAllocator, WindowHandle handle);
