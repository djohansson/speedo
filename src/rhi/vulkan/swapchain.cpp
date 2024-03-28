#include "../swapchain.h"

#include "utils.h"

#include <format>

template <>
RenderPassBeginInfo<Vk> Swapchain<Vk>::begin(CommandBufferHandle<Vk> cmd, SubpassContents<Vk> contents)
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
std::tuple<bool, uint32_t, uint32_t> Swapchain<Vk>::flip()
{
	ZoneScoped;

	auto& device = *getDevice();

	static constexpr std::string_view flipFrameStr = "";

	auto lastFrameIndex = myFrameIndex;
	
	Fence<Vk> fence(getDevice(), FenceCreateDesc<Vk>{});

	auto flipResult = checkFlipOrPresentResult(vkAcquireNextImageKHR(
		device,
		mySwapchain,
		UINT64_MAX,
		VK_NULL_HANDLE,
		fence,
		&myFrameIndex));

	auto& newFrame = myFrames[myFrameIndex];

	newFrame.getFence().swap(fence);

	auto zoneNameStr = std::format(
		"Swapchain::flip frame:{0}",
		flipResult == VK_SUCCESS ? myFrameIndex : ~0u);

	ZoneName(zoneNameStr.c_str(), zoneNameStr.size());

	return std::make_tuple(flipResult == VK_SUCCESS, lastFrameIndex, myFrameIndex);
}

template <>
QueuePresentInfo<Vk> Swapchain<Vk>::preparePresent(const QueueHostSyncInfo<Vk>& hostSyncInfo)
{
	ZoneScopedN("Swapchain::preparePresent");

	auto presentInfo = myFrames[myFrameIndex].preparePresent(hostSyncInfo);
	presentInfo.swapchains.push_back(mySwapchain);

	return presentInfo;
}

template <>
void Swapchain<Vk>::internalCreateSwapchain(
	const SwapchainConfiguration<Vk>& config, SwapchainHandle<Vk> previous)
{
	ZoneScopedN("Swapchain::internalCreateSwapchain");

	auto& device = *getDevice();

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
		device,
		&info,
		&device.getInstance()->getHostAllocationCallbacks(),
		&mySwapchain));

	if (previous)
	{
#if GRAPHICS_VALIDATION_ENABLED
		device.eraseOwnedObjectHandle(getUid(), reinterpret_cast<uint64_t>(previous));
#endif

		vkDestroySwapchainKHR(
			device,
			previous,
			&device.getInstance()->getHostAllocationCallbacks());
	}

#if GRAPHICS_VALIDATION_ENABLED
	device.addOwnedObjectHandle(
		getUid(),
		VK_OBJECT_TYPE_SWAPCHAIN_KHR,
		reinterpret_cast<uint64_t>(mySwapchain),
		std::format("{0}_Swapchain", getName()));
#endif

	uint32_t frameCount = config.imageCount;

	assert(frameCount);

	uint32_t imageCount;
	VK_CHECK(vkGetSwapchainImagesKHR(
		device, mySwapchain, &imageCount, nullptr));

	assert(imageCount == frameCount);

	std::vector<ImageHandle<Vk>> colorImages(imageCount);
	VK_CHECK(vkGetSwapchainImagesKHR(
		device, mySwapchain, &imageCount, colorImages.data()));

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

	myFrameIndex = frameCount - 1;
}

template <>
Swapchain<Vk>::Swapchain(Swapchain&& other) noexcept
	: DeviceObject(std::forward<Swapchain>(other))
	, myDesc(std::exchange(other.myDesc, {}))
	, mySwapchain(std::exchange(other.mySwapchain, {}))
	, myFrames(std::exchange(other.myFrames, {}))
	, myFrameIndex(std::exchange(other.myFrameIndex, 0))
{}

template <>
Swapchain<Vk>::Swapchain(
	const std::shared_ptr<Device<Vk>>& device,
	const SwapchainConfiguration<Vk>& config,
	SurfaceHandle<Vk>&& surface,
	SwapchainHandle<Vk> previous)
	: DeviceObject(device, {})
	, myDesc{config.extent} // more?
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
}
