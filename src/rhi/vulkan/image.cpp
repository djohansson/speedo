#include <rhi/device.h>
#include <rhi/image.h>
#include <rhi/rhi.h>
#include <rhi/vulkan/utils.h>

#include <tuple>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(Image<kVk>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(Image<kVk>);
IMPLEMENT_OBJECT_GETINSTANCE(ImageView<kVk>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(ImageView<kVk>);

namespace image
{

namespace detail
{

std::tuple<VkImage, VmaAllocation>
CreateImage2D(VmaAllocator allocator, const ImageCreateDesc<kVk>& desc)
{
	return CreateImage2D(
		allocator,
		desc.mipLevels[0].extent.width,
		desc.mipLevels[0].extent.height,
		desc.mipLevels.size(),
		vk::ToVk(desc.format),
		vk::ToVk(desc.tiling),
		vk::ToVk(desc.usageFlags),
		vk::ToVk(desc.memoryFlags),
		nullptr,
		vk::ToVk(desc.layout));
}

std::tuple<VkImage, VmaAllocation> CreateImage2D(
	VkCommandBuffer cmd, VmaAllocator allocator, VkBuffer buffer, const ImageCreateDesc<kVk>& desc)
{
	return CreateImage2D(
		cmd,
		allocator,
		buffer,
		desc.mipLevels[0].extent.width,
		desc.mipLevels[0].extent.height,
		desc.mipLevels.size(),
		&desc.mipLevels[0].offset,
		sizeof(desc.mipLevels[0]) / sizeof(uint32_t),
		vk::ToVk(desc.format),
		vk::ToVk(desc.tiling),
		vk::ToVk(desc.usageFlags),
		vk::ToVk(desc.memoryFlags),
		vk::ToVk(desc.imageAspectFlags),
		nullptr,
		vk::ToVk(desc.layout));
}


} // namespace detail

} // namespace image

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
			vk::ToVk(GetDesc().format),
			vk::ToVk(GetDesc().layout),
			vk::ToVk(layout),
			GetDesc().mipLevels.size(),
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

	const auto& from = source.GetDesc().mipLevels[0].extent;
	const auto& to = GetDesc().mipLevels[0].extent;
	VkImageBlit blit{
		.srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
		.srcOffsets = {{0, 0, 0}, {static_cast<int32_t>(from.width), static_cast<int32_t>(from.height), 1}},
		.dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
		.dstOffsets = {{0, 0, 0}, {static_cast<int32_t>(to.width), static_cast<int32_t>(to.height), 1}}};
	vkCmdBlitImage(
		cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, *this, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
}

template <>
void Image<kVk>::GenerateMips(CommandBufferHandle<kVk> cmd, ImageLayout layout)
{
	ZoneScopedN("Image::GenerateMips");

	ENSURE(GetDesc().layout == ImageLayout::kTransferDestination);
	auto levelCount = static_cast<uint32_t>(GetDesc().mipLevels.size());
	for (uint32_t level = 1; level < levelCount; level++)
	{
		// the level above becomes the source
		TransitionImageLayout(
			cmd, *this, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, level - 1, 1, VK_IMAGE_ASPECT_COLOR_BIT);
		const auto& from = GetDesc().mipLevels[level - 1].extent;
		const auto& to = GetDesc().mipLevels[level].extent;
		VkImageBlit blit{
			.srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = level - 1, .baseArrayLayer = 0, .layerCount = 1},
			.srcOffsets = {{0, 0, 0}, {static_cast<int32_t>(from.width), static_cast<int32_t>(from.height), 1}},
			.dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = level, .baseArrayLayer = 0, .layerCount = 1},
			.dstOffsets = {{0, 0, 0}, {static_cast<int32_t>(to.width), static_cast<int32_t>(to.height), 1}}};
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
		[&desc, &memory, offset]
		{
			auto info = ImageCreateInfo2D(
				desc.mipLevels[0].extent.width,
				desc.mipLevels[0].extent.height,
				static_cast<uint32_t>(desc.mipLevels.size()),
				vk::ToVk(desc.format),
				vk::ToVk(desc.tiling),
				vk::ToVk(desc.usageFlags),
				vk::ToVk(desc.layout));
			return ValueType{
				CreateAliasingImage(memory.GetAllocator(), memory.GetAllocation(), offset, info, GetDebugName(desc).c_str()),
				VK_NULL_HANDLE};
		}())
{}

template <>
MemoryRequirements Image<kVk>::GetMemoryRequirements(const Device<kVk>& device, const CreateDescType& desc)
{
	auto requirements = ::GetImageMemoryRequirements(
		device.GetAllocator(),
		ImageCreateInfo2D(
			desc.mipLevels[0].extent.width,
			desc.mipLevels[0].extent.height,
			static_cast<uint32_t>(desc.mipLevels.size()),
			vk::ToVk(desc.format),
			vk::ToVk(desc.tiling),
			vk::ToVk(desc.usageFlags),
			vk::ToVk(desc.layout)));
	return {.size = requirements.size, .alignment = requirements.alignment, .memoryTypeBits = requirements.memoryTypeBits};
}

template <>
Image<kVk>::Image(CreateDescType&& desc)
	: Image(
		std::forward<CreateDescType>(desc),
		image::detail::CreateImage2D(GetDevice(desc.device).GetAllocator(), desc))
{}

template <>
Image<kVk>::Image(
	CreateDescType&& desc,
	CommandBufferHandle<kVk> cmd,
	core::TaskCreateInfo<void>& timlineCallbackOut,
	std::tuple<BufferHandle<kVk>, AllocationHandle<kVk>>&& initialData)
	: Image(
		std::forward<CreateDescType>(desc),
		image::detail::CreateImage2D(
			cmd,
			GetDevice(desc.device).GetAllocator(),
			std::get<0>(initialData),
			desc))
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
			nullptr))
{}

template <>
Image<kVk>::Image(
	CreateDescType&& desc,
	Buffer<kVk>&& staging,
	CommandBufferHandle<kVk> cmd,
	core::TaskCreateInfo<void>& timlineCallbackOut)
	: Image(
		std::forward<CreateDescType>(desc),
		image::detail::CreateImage2D(cmd, GetDevice(desc.device).GetAllocator(), staging.GetBuffer(), desc))
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
		CreateImageView2D(
			desc.device,
			&GetInstance().GetHostAllocationCallbacks(),
			0, // "reserved for future use"
			desc.image,
			vk::ToVk(desc.format),
			vk::ToVk(desc.aspectFlags),
			desc.levelCount == 0 ? VK_REMAINING_MIP_LEVELS : desc.levelCount,
			GetDebugName(desc),
			VkComponentMapping{
				.r = vk::ToVk(desc.components[0]),
				.g = vk::ToVk(desc.components[1]),
				.b = vk::ToVk(desc.components[2]),
				.a = vk::ToVk(desc.components[3])},
			desc.baseLevel))
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
