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
		desc.format,
		desc.tiling,
		desc.usageFlags,
		desc.memoryFlags,
		nullptr,
		desc.layout);
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
		desc.format,
		desc.tiling,
		desc.usageFlags,
		desc.memoryFlags,
		desc.imageAspectFlags,
		nullptr,
		desc.layout);
}


} // namespace detail

} // namespace image

template <>
void Image<kVk>::Transition(CommandBufferHandle<kVk> cmd, ImageLayout<kVk> layout, ImageAspectFlags<kVk> aspectFlags)
{
	ZoneScopedN("Image::Transition");

	if (aspectFlags == VK_IMAGE_ASPECT_NONE)
	{
		if (HasColorComponent(GetDesc().format))
			aspectFlags |= VK_IMAGE_ASPECT_COLOR_BIT;
		else
		{
			if (HasDepthComponent(GetDesc().format))
				aspectFlags |= VK_IMAGE_ASPECT_DEPTH_BIT;
			if (HasStencilComponent(GetDesc().format))
				aspectFlags |= VK_IMAGE_ASPECT_STENCIL_BIT;
		}
	}

	if (GetDesc().layout != layout || GetDesc().imageAspectFlags != aspectFlags)
	{
		TransitionImageLayout(
			cmd, *this, GetDesc().format, GetDesc().layout, layout, GetDesc().mipLevels.size(), aspectFlags);
		InternalSetImageLayout(layout);
		InternalSetAspectFlags(aspectFlags);
	}
}

template <>
void Image<kVk>::Clear(
	CommandBufferHandle<kVk> cmd,
	const ClearValue<kVk>& value,
	const std::optional<ImageSubresourceRange<kVk>>& range)
{
	ZoneScopedN("Image::clear");

	static const VkImageSubresourceRange kDefaultRange{
		.aspectMask = GetDesc().imageAspectFlags,
		.baseMipLevel = 0,
		.levelCount = VK_REMAINING_MIP_LEVELS,
		.baseArrayLayer = 0,
		.layerCount = VK_REMAINING_ARRAY_LAYERS};

	if ((GetDesc().imageAspectFlags & VK_IMAGE_ASPECT_COLOR_BIT) != 0U)
	{
		vkCmdClearColorImage(
			cmd,
			static_cast<VkImage>(*this),
			GetDesc().layout,
			&value.color,
			1,
			range ? &range.value() : &kDefaultRange);
	}
	else if (((GetDesc().imageAspectFlags & VK_IMAGE_ASPECT_DEPTH_BIT) != 0U) || ((GetDesc().imageAspectFlags & VK_IMAGE_ASPECT_STENCIL_BIT) != 0U))
	{
		vkCmdClearDepthStencilImage(
			cmd,
			static_cast<VkImage>(*this),
			GetDesc().layout,
			&value.depthStencil,
			1,
			range ? &range.value() : &kDefaultRange);
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
			desc.format,
			desc.aspectFlags,
			1,
			GetDebugName(desc)))
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
