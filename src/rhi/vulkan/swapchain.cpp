#include "../swapchain.h"

#include "utils.h"

#include <format>

template <>
const std::optional<RenderPassBeginInfo<Vk>>&
Swapchain<Vk>::begin(CommandBufferHandle<Vk> cmd, SubpassContents<Vk> contents)
{
	return myFrames[myFrameIndex].begin(cmd, contents);
}

template <>
void Swapchain<Vk>::end(CommandBufferHandle<Vk> cmd)
{
	return myFrames[myFrameIndex].end(cmd);
}

template <>
const RenderTargetCreateDesc<Vk>& Swapchain<Vk>::getRenderTargetDesc() const
{
	return myDesc;
}

template <>
ImageLayout<Vk> Swapchain<Vk>::getColorImageLayout(uint32_t index) const
{
	return myFrames[myFrameIndex].getColorImageLayout(index);
}

template <>
ImageLayout<Vk> Swapchain<Vk>::getDepthStencilImageLayout() const
{
	return myFrames[myFrameIndex].getDepthStencilImageLayout();
}

template <>
void Swapchain<Vk>::blit(
	CommandBufferHandle<Vk> cmd,
	const IRenderTarget<Vk>& srcRenderTarget,
	const ImageSubresourceLayers<Vk>& srcSubresource,
	uint32_t srcIndex,
	const ImageSubresourceLayers<Vk>& dstSubresource,
	uint32_t dstIndex,
	Filter<Vk> filter)
{
	myFrames[myFrameIndex].blit(
		cmd, srcRenderTarget, srcSubresource, srcIndex, dstSubresource, dstIndex, filter);
}

template <>
void Swapchain<Vk>::clearSingleAttachment(
	CommandBufferHandle<Vk> cmd, const ClearAttachment<Vk>& clearAttachment) const
{
	myFrames[myFrameIndex].clearSingleAttachment(cmd, clearAttachment);
}

template <>
void Swapchain<Vk>::clearAllAttachments(
	CommandBufferHandle<Vk> cmd,
	const ClearColorValue<Vk>& color,
	const ClearDepthStencilValue<Vk>& depthStencil) const
{
	myFrames[myFrameIndex].clearAllAttachments(cmd, color, depthStencil);
}

template <>
void Swapchain<Vk>::clearColor(
	CommandBufferHandle<Vk> cmd, const ClearColorValue<Vk>& color, uint32_t index)
{
	myFrames[myFrameIndex].clearColor(cmd, color, index);
}

template <>
void Swapchain<Vk>::clearDepthStencil(
	CommandBufferHandle<Vk> cmd, const ClearDepthStencilValue<Vk>& depthStencil)
{
	myFrames[myFrameIndex].clearDepthStencil(cmd, depthStencil);
}

template <>
void Swapchain<Vk>::transitionColor(
	CommandBufferHandle<Vk> cmd, ImageLayout<Vk> layout, uint32_t index)
{
	myFrames[myFrameIndex].transitionColor(cmd, layout, index);
}

template <>
void Swapchain<Vk>::transitionDepthStencil(CommandBufferHandle<Vk> cmd, ImageLayout<Vk> layout)
{
	myFrames[myFrameIndex].transitionDepthStencil(cmd, layout);
}

template <>
std::tuple<bool, uint64_t> Swapchain<Vk>::flip()
{
	ZoneScoped;

	static constexpr std::string_view flipFrameStr = "";

	const auto& lastFrame = myFrames[myLastFrameIndex];

	auto flipResult = checkFlipOrPresentResult(vkAcquireNextImageKHR(
		*getDevice(),
		mySwapchain,
		UINT64_MAX,
		lastFrame.getNewImageAcquiredSemaphore(),
		0,
		&myFrameIndex));

	if (flipResult != VK_SUCCESS)
		return std::make_tuple(false, lastFrame.getLastPresentTimelineValue());

	auto zoneNameStr = std::format("Swapchain::flip frame:{0}", myFrameIndex);

	ZoneName(zoneNameStr.c_str(), zoneNameStr.size());

	const auto& frame = myFrames[myFrameIndex];

	return std::make_tuple(true, frame.getLastPresentTimelineValue());
}

template <>
QueuePresentInfo<Vk> Swapchain<Vk>::preparePresent(uint64_t timelineValue)
{
	ZoneScopedN("Swapchain::preparePresent");

	myLastFrameIndex = myFrameIndex;

	auto presentInfo = myFrames[myFrameIndex].preparePresent(timelineValue);
	presentInfo.swapchains.push_back(mySwapchain);

	return presentInfo;
}

template <>
void Swapchain<Vk>::internalCreateSwapchain(
	const SwapchainConfiguration<Vk>& config, SwapchainHandle<Vk> previous)
{
	ZoneScopedN("Swapchain::internalCreateSwapchain");

	myDesc = {config.extent}; // more?

	VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
	info.surface = mySurface;
	info.minImageCount = config.imageCount;
	info.imageFormat = config.surfaceFormat.format;
	info.imageColorSpace = config.surfaceFormat.colorSpace;
	info.imageExtent = config.extent;
	info.imageArrayLayers = 1;
	info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	info.presentMode = config.presentMode;
	info.clipped = VK_TRUE;
	info.oldSwapchain = previous;

	VK_CHECK(vkCreateSwapchainKHR(
		*getDevice(),
		&info,
		&getDevice()->getInstance()->getHostAllocationCallbacks(),
		&mySwapchain));

	if (previous)
	{
#if GRAPHICS_VALIDATION_ENABLED
		getDevice()->eraseOwnedObjectHandle(getUid(), reinterpret_cast<uint64_t>(previous));
#endif

		vkDestroySwapchainKHR(
			*getDevice(),
			previous,
			&getDevice()->getInstance()->getHostAllocationCallbacks());
	}

#if GRAPHICS_VALIDATION_ENABLED
	getDevice()->addOwnedObjectHandle(
		getUid(),
		VK_OBJECT_TYPE_SWAPCHAIN_KHR,
		reinterpret_cast<uint64_t>(mySwapchain),
		std::format("{0}_Swapchain", getName()));
#endif

	uint32_t frameCount = config.imageCount;

	assert(frameCount);

	uint32_t imageCount;
	VK_CHECK(vkGetSwapchainImagesKHR(
		*getDevice(), mySwapchain, &imageCount, nullptr));

	assert(imageCount == frameCount);

	std::vector<ImageHandle<Vk>> colorImages(imageCount);
	VK_CHECK(vkGetSwapchainImagesKHR(
		*getDevice(), mySwapchain, &imageCount, colorImages.data()));

	myFrames.clear();
	myFrames.reserve(frameCount);

	for (uint32_t frameIt = 0ul; frameIt < frameCount; frameIt++)
		myFrames.emplace_back(Frame<Vk>(
			getDevice(),
			FrameCreateDesc<Vk>{
				{config.extent,
				{config.surfaceFormat.format},
				{VK_IMAGE_LAYOUT_UNDEFINED},
				{colorImages[frameIt]}},
				frameIt}));

	myLastFrameIndex = frameCount - 1;
}

template <>
Swapchain<Vk>::Swapchain(Swapchain&& other) noexcept
	: DeviceObject(std::forward<Swapchain>(other))
	, myDesc(std::exchange(other.myDesc, {}))
	, mySwapchain(std::exchange(other.mySwapchain, {}))
	, myFrames(std::exchange(other.myFrames, {}))
	, myFrameIndex(std::exchange(other.myFrameIndex, 0))
	, myLastFrameIndex(std::exchange(other.myLastFrameIndex, 0))
{}

template <>
Swapchain<Vk>::Swapchain(
	const std::shared_ptr<Device<Vk>>& device,
	const SwapchainConfiguration<Vk>& config,
	SurfaceHandle<Vk>&& surface,
	SwapchainHandle<Vk> previous)
	: DeviceObject(device, {})
	, mySurface(std::forward<SurfaceHandle<Vk>>(surface))
{
	ZoneScopedN("Swapchain()");

	internalCreateSwapchain(config, previous);
}

template <>
Swapchain<Vk>::~Swapchain()
{
	ZoneScopedN("~Swapchain()");

	if (mySwapchain)
		vkDestroySwapchainKHR(
			*getDevice(),
			mySwapchain,
			&getDevice()->getInstance()->getHostAllocationCallbacks());

	if (mySurface)
		vkDestroySurfaceKHR(
			*getDevice()->getInstance(),
			mySurface,
			&getDevice()->getInstance()->getHostAllocationCallbacks());
}

template <>
Swapchain<Vk>& Swapchain<Vk>::operator=(Swapchain&& other) noexcept
{
	DeviceObject::operator=(std::forward<Swapchain>(other));
	myDesc = std::exchange(other.myDesc, {});
	mySwapchain = std::exchange(other.mySwapchain, {});
	myFrames = std::exchange(other.myFrames, {});
	myFrameIndex = std::exchange(other.myFrameIndex, 0);
	myLastFrameIndex = std::exchange(other.myLastFrameIndex, 0);
	return *this;
}

template <>
void Swapchain<Vk>::swap(Swapchain& rhs) noexcept
{
	DeviceObject::swap(rhs);
	std::swap(myDesc, rhs.myDesc);
	std::swap(mySwapchain, rhs.mySwapchain);
	std::swap(myFrames, rhs.myFrames);
	std::swap(myFrameIndex, rhs.myFrameIndex);
	std::swap(myLastFrameIndex, rhs.myLastFrameIndex);
}
