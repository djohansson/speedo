#include <rhi/device.h>
#include <rhi/image.h>
#include <rhi/rhi.h>
#include <rhi/vulkan/utils.h>

#include <tuple>
#include <utility>
#include <vector>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(Image<kVk>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(Image<kVk>);
IMPLEMENT_OBJECT_GETINSTANCE(ImageView<kVk>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(ImageView<kVk>);

namespace
{

// the far corner of a level, for a blit
VkOffset3D LevelEnd(const ImageCreateDesc<kVk>& desc, uint32_t level)
{
	auto extent = desc.LevelExtent(level);
	return {.x = static_cast<int32_t>(extent.width), .y = static_cast<int32_t>(extent.height), .z = static_cast<int32_t>(extent.depth)};
}

VkImageCreateInfo CreateInfo(const ImageCreateDesc<kVk>& desc)
{
	return ImageCreateInfo(
		vk::ToVk(desc.extent),
		desc.levelCount,
		vk::ToVk(desc.format),
		vk::ToVk(desc.tiling),
		vk::ToVk(desc.usageFlags),
		vk::ToVk(desc.layout));
}

// an image of desc whose levels are copied from buffer (byteCount bytes), where desc's levelOffsets have them (else
// packed), leaving them in VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
std::tuple<VkImage, VmaAllocation> CreateUploadedImage(
	VkCommandBuffer cmd, VmaAllocator allocator, VkBuffer buffer, uint64_t byteCount, const ImageCreateDesc<kVk>& desc)
{
	ENSURE(buffer != VK_NULL_HANDLE);
	ENSUREF(
		desc.levelOffsets.empty() || desc.levelOffsets.size() == desc.levelCount,
		"{}: an upload of {} levels was given {} offsets",
		GetDebugName(desc),
		desc.levelCount,
		desc.levelOffsets.size());
	ENSUREF(
		BlockOf(desc.format).byteCount > 0,
		"{}: format {} can't be uploaded (its texels aren't one run of bytes)",
		GetDebugName(desc),
		std::to_underlying(desc.format));

	auto info = CreateInfo(desc);
	info.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	auto result = CreateImage(allocator, info, vk::ToVk(desc.memoryFlags), nullptr);
	auto image = std::get<0>(result);

	TransitionImageLayout(
		cmd, image, info.initialLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, info.mipLevels, vk::ToVk(desc.imageAspectFlags));

	std::vector<VkBufferImageCopy> regions;
	regions.reserve(info.mipLevels);
	uint64_t packedOffset = 0;
	for (uint32_t level = 0; level < info.mipLevels; level++)
	{
		auto extent = desc.LevelExtent(level);
		auto levelByteCount = PackedByteCount(desc.format, extent);
		auto offset = desc.levelOffsets.empty() ? packedOffset : desc.levelOffsets[level];
		ENSUREF(
			offset + levelByteCount <= byteCount,
			"{}: level {} ({} bytes from {}) runs past the source's {} bytes",
			GetDebugName(desc),
			level,
			levelByteCount,
			offset,
			byteCount);
		packedOffset += levelByteCount;
		regions.push_back({
			.bufferOffset = offset,
			.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = level, .baseArrayLayer = 0, .layerCount = 1},
			.imageExtent = vk::ToVk(extent)});
	}
	vkCmdCopyBufferToImage(cmd, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, regions.size(), regions.data());

	return result;
}

} // namespace

template <>
void Image<kVk>::Transition(CommandBufferHandle<kVk> cmd, ImageLayout layout, ImageAspect aspectFlags)
{
	ZoneScopedN("Image::Transition");

	if (aspectFlags == ImageAspect::kNone)
		aspectFlags = AspectOf(GetDesc().format);

	if (GetDesc().layout != layout || GetDesc().imageAspectFlags != aspectFlags)
	{
		TransitionImageLayout(
			cmd,
			*this,
			vk::ToVk(GetDesc().layout),
			vk::ToVk(layout),
			0,
			GetDesc().levelCount,
			vk::ToVk(aspectFlags));
		InternalSetImageLayout(layout);
		InternalSetAspectFlags(aspectFlags);
	}
}

template <>
void Image<kVk>::BlitFrom(CommandBufferHandle<kVk> cmd, const Image& source)
{
	ZoneScopedN("Image::BlitFrom");

	ENSURE(source.GetDesc().layout == ImageLayout::kTransferSource);
	Transition(cmd, ImageLayout::kTransferDestination, ImageAspect::kColor);

	VkImageBlit blit{
		.srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
		.srcOffsets = {{0, 0, 0}, LevelEnd(source.GetDesc(), 0)},
		.dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
		.dstOffsets = {{0, 0, 0}, LevelEnd(GetDesc(), 0)}};
	vkCmdBlitImage(
		cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, *this, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
}

template <>
void Image<kVk>::GenerateMips(CommandBufferHandle<kVk> cmd, ImageLayout layout)
{
	ZoneScopedN("Image::GenerateMips");

	ENSURE(GetDesc().layout == ImageLayout::kTransferDestination);
	auto levelCount = GetDesc().levelCount;
	for (uint32_t level = 1; level < levelCount; level++)
	{
		// the level above becomes the source
		TransitionImageLayout(
			cmd, *this, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, level - 1, 1, VK_IMAGE_ASPECT_COLOR_BIT);
		VkImageBlit blit{
			.srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = level - 1, .baseArrayLayer = 0, .layerCount = 1},
			.srcOffsets = {{0, 0, 0}, LevelEnd(GetDesc(), level - 1)},
			.dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = level, .baseArrayLayer = 0, .layerCount = 1},
			.dstOffsets = {{0, 0, 0}, LevelEnd(GetDesc(), level)}};
		vkCmdBlitImage(
			cmd, *this, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, *this, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
	}
	// all but the last are sources now
	if (levelCount > 1)
		TransitionImageLayout(
			cmd, *this, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk::ToVk(layout), 0, levelCount - 1, VK_IMAGE_ASPECT_COLOR_BIT);
	TransitionImageLayout(
		cmd, *this, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, vk::ToVk(layout), levelCount - 1, 1, VK_IMAGE_ASPECT_COLOR_BIT);
	InternalSetImageLayout(layout);
	InternalSetAspectFlags(ImageAspect::kColor);
}

template <>
void Image<kVk>::Clear(
	CommandBufferHandle<kVk> cmd,
	const ClearValue& value,
	const std::optional<ImageSubresourceRange<kVk>>& range)
{
	ZoneScopedN("Image::clear");

	// not static: it depends on the image
	const VkImageSubresourceRange defaultRange{
		.aspectMask = vk::ToVk(GetDesc().imageAspectFlags),
		.baseMipLevel = 0,
		.levelCount = VK_REMAINING_MIP_LEVELS,
		.baseArrayLayer = 0,
		.layerCount = VK_REMAINING_ARRAY_LAYERS};
	auto clearValue = vk::ToVk(value, GetDesc().imageAspectFlags);

	if (Any(GetDesc().imageAspectFlags & ImageAspect::kColor))
	{
		vkCmdClearColorImage(
			cmd,
			static_cast<VkImage>(*this),
			vk::ToVk(GetDesc().layout),
			&clearValue.color,
			1,
			range ? &range.value() : &defaultRange);
	}
	else if (Any(GetDesc().imageAspectFlags & (ImageAspect::kDepth | ImageAspect::kStencil)))
	{
		vkCmdClearDepthStencilImage(
			cmd,
			static_cast<VkImage>(*this),
			vk::ToVk(GetDesc().layout),
			&clearValue.depthStencil,
			1,
			range ? &range.value() : &defaultRange);
	}
	else
	{
		ENSURE(false); // Unsupported aspect flags.
	}
}

template <>
void Image<kVk>::Swap(Image& rhs) noexcept
{
	DeviceObject<Image<kVk>>::Swap(rhs);
	std::swap(myImage, rhs.myImage);
}

template <>
Image<kVk>::Image(Image&& other) noexcept
{
	Swap(other);
}

template <>
Image<kVk>::Image(CreateDescType&& desc, ValueType&& data)
	: DeviceObject<Image<kVk>>(std::forward<CreateDescType>(desc))
	, myImage(std::forward<ValueType>(data))
{}

template <>
Image<kVk>::Image(CreateDescType&& desc, const MemoryBlock<kVk>& memory, uint64_t offset)
	: Image(
		std::forward<CreateDescType>(desc),
		ValueType{
			CreateAliasingImage(memory.GetAllocator(), memory.GetAllocation(), offset, CreateInfo(desc), GetDebugName(desc).c_str()),
			VK_NULL_HANDLE})
{}

template <>
MemoryRequirements Image<kVk>::GetMemoryRequirements(const Device<kVk>& device, const CreateDescType& desc)
{
	auto requirements = ::GetImageMemoryRequirements(device.GetAllocator(), CreateInfo(desc));
	return {.size = requirements.size, .alignment = requirements.alignment, .memoryTypeBits = requirements.memoryTypeBits};
}

template <>
Image<kVk>::Image(CreateDescType&& desc)
	: Image(
		std::forward<CreateDescType>(desc),
		CreateImage(GetDevice(desc.device).GetAllocator(), CreateInfo(desc), vk::ToVk(desc.memoryFlags), nullptr))
{}

template <>
Image<kVk>::Image(
	CreateDescType&& desc,
	CommandBufferHandle<kVk> cmd,
	core::TaskCreateInfo<void>& timlineCallbackOut,
	std::tuple<BufferHandle<kVk>, AllocationHandle<kVk>>&& initialData,
	uint64_t byteCount)
	: Image(
		std::forward<CreateDescType>(desc),
		CreateUploadedImage(cmd, GetDevice(desc.device).GetAllocator(), std::get<0>(initialData), byteCount, desc))
{
	InternalSetImageLayout(ImageLayout::kTransferDestination); // as the upload leaves it
	timlineCallbackOut = core::CreateTask(
		[allocator = GetDevice().GetAllocator(), buffer = std::get<0>(initialData), memory = std::get<1>(initialData)]{
			DestroyBuffer(allocator, buffer, memory); });
}

template <>
Image<kVk>::Image(
	CreateDescType&& desc,
	CommandBufferHandle<kVk> cmd,
	const void* initialData,
	size_t initialDataSize,
	core::TaskCreateInfo<void>& timlineCallbackOut)
	: Image(
		std::forward<CreateDescType>(desc),
		cmd,
		timlineCallbackOut,
		CreateStagingBuffer(
			GetDevice(desc.device).GetAllocator(),
			initialData,
			initialDataSize,
			nullptr),
		initialDataSize)
{}

template <>
Image<kVk>::Image(
	CreateDescType&& desc,
	Buffer<kVk>&& staging,
	CommandBufferHandle<kVk> cmd,
	core::TaskCreateInfo<void>& timlineCallbackOut)
	: Image(
		std::forward<CreateDescType>(desc),
		CreateUploadedImage(cmd, GetDevice(desc.device).GetAllocator(), staging.GetBuffer(), staging.GetDesc().size, desc))
{
	InternalSetImageLayout(ImageLayout::kTransferDestination); // as the upload leaves it
	timlineCallbackOut = core::CreateTask([staging = std::make_shared<Buffer<kVk>>(std::move(staging))] {});
}

template <>
Image<kVk>::~Image()
{
	if (IsValid())
		DestroyImage(
			GetDevice().GetAllocator(),
			std::get<0>(myImage),
			std::get<1>(myImage));
}

template <>
Image<kVk>& Image<kVk>::operator=(Image<kVk>&& other) noexcept
{
	Swap(other);
	return *this;
}

template <>
void ImageView<kVk>::Swap(ImageView& rhs) noexcept
{
	DeviceObject<ImageView<kVk>>::Swap(rhs);
	std::swap(myView, rhs.myView);
}

template <>
ImageView<kVk>::ImageView(ImageView&& other) noexcept
{
	Swap(other);
}

template <>
ImageView<kVk>::ImageView(
	CreateDescType&& desc,
	ImageViewHandle<kVk>&& view)
	: DeviceObject<ImageView<kVk>>(std::forward<CreateDescType>(desc))
	, myView(std::forward<ImageViewHandle<kVk>>(view))
{}

template <>
ImageView<kVk>::ImageView(
	CreateDescType&& desc)
	: ImageView<kVk>(
		std::forward<CreateDescType>(desc),
		CreateImageView(
			desc.device,
			&GetInstance().GetHostAllocationCallbacks(),
			0, // "reserved for future use"
			desc.image.handle,
			vk::ToVk(desc.format),
			vk::ToVk(desc.aspectFlags),
			desc.levelCount == 0 ? VK_REMAINING_MIP_LEVELS : desc.levelCount,
			GetDebugName(desc),
			VkComponentMapping{
				.r = vk::ToVk(desc.components[0]),
				.g = vk::ToVk(desc.components[1]),
				.b = vk::ToVk(desc.components[2]),
				.a = vk::ToVk(desc.components[3])},
			desc.baseLevel,
			desc.image.volume ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D))
{}

template <>
ImageView<kVk>::~ImageView()
{
	if (IsValid())
		DestroyImageView(GetDevice(), &GetInstance().GetHostAllocationCallbacks(), myView);
}

template <>
ImageView<kVk>& ImageView<kVk>::operator=(ImageView&& other) noexcept
{
	Swap(other);
	return *this;
}

} // namespace rhi
