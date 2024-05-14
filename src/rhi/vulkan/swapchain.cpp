#include "../swapchain.h"

#include "utils.h"

#include <format>

template <>
RenderPassBeginInfo<Vk> Swapchain<Vk>::Begin(CommandBufferHandle<Vk> cmd, SubpassContents<Vk> contents)
{
	return myFrames[myFrameIndex].Begin(cmd, contents);
}

template <>
void Swapchain<Vk>::End(CommandBufferHandle<Vk> cmd)
{
	return myFrames[myFrameIndex].End(cmd);
}

template <>
const RenderTargetCreateDesc<Vk>& Swapchain<Vk>::GetRenderTargetDesc() const
{
	return myDesc;
}

template <>
ImageLayout<Vk> Swapchain<Vk>::GetColorImageLayout(uint32_t index) const
{
	return myFrames[myFrameIndex].GetColorImageLayout(index);
}

template <>
ImageLayout<Vk> Swapchain<Vk>::GetDepthStencilImageLayout() const
{
	return myFrames[myFrameIndex].GetDepthStencilImageLayout();
}

template <>
void Swapchain<Vk>::Blit(
	CommandBufferHandle<Vk> cmd,
	const IRenderTarget<Vk>& srcRenderTarget,
	const ImageSubresourceLayers<Vk>& srcSubresource,
	uint32_t srcIndex,
	const ImageSubresourceLayers<Vk>& dstSubresource,
	uint32_t dstIndex,
	Filter<Vk> filter)
{
	myFrames[myFrameIndex].Blit(
		cmd, srcRenderTarget, srcSubresource, srcIndex, dstSubresource, dstIndex, filter);
}

template <>
void Swapchain<Vk>::ClearSingleAttachment(
	CommandBufferHandle<Vk> cmd, const ClearAttachment<Vk>& clearAttachment) const
{
	myFrames[myFrameIndex].ClearSingleAttachment(cmd, clearAttachment);
}

template <>
void Swapchain<Vk>::ClearAllAttachments(
	CommandBufferHandle<Vk> cmd,
	const ClearColorValue<Vk>& color,
	const ClearDepthStencilValue<Vk>& depthStencil) const
{
	myFrames[myFrameIndex].ClearAllAttachments(cmd, color, depthStencil);
}

template <>
void Swapchain<Vk>::ClearColor(
	CommandBufferHandle<Vk> cmd, const ClearColorValue<Vk>& color, uint32_t index)
{
	myFrames[myFrameIndex].ClearColor(cmd, color, index);
}

template <>
void Swapchain<Vk>::ClearDepthStencil(
	CommandBufferHandle<Vk> cmd, const ClearDepthStencilValue<Vk>& depthStencil)
{
	myFrames[myFrameIndex].ClearDepthStencil(cmd, depthStencil);
}

template <>
void Swapchain<Vk>::TransitionColor(
	CommandBufferHandle<Vk> cmd, ImageLayout<Vk> layout, uint32_t index)
{
	myFrames[myFrameIndex].TransitionColor(cmd, layout, index);
}

template <>
void Swapchain<Vk>::TransitionDepthStencil(CommandBufferHandle<Vk> cmd, ImageLayout<Vk> layout)
{
	myFrames[myFrameIndex].TransitionDepthStencil(cmd, layout);
}

template <>
std::tuple<bool, uint32_t, uint32_t> Swapchain<Vk>::Flip()
{
	ZoneScoped;

	auto& device = *getDevice();

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

	newFrame.GetFence().Swap(fence);

	auto zoneNameStr =
		std::format("Swapchain::flip frame:{0}", flipResult == VK_SUCCESS ? myFrameIndex : ~0U);

	ZoneName(zoneNameStr.c_str(), zoneNameStr.size());

	return std::make_tuple(flipResult == VK_SUCCESS, lastFrameIndex, myFrameIndex);
}

template <>
QueuePresentInfo<Vk> Swapchain<Vk>::PreparePresent(const QueueHostSyncInfo<Vk>& hostSyncInfo)
{
	ZoneScopedN("Swapchain::PreparePresent");

	auto presentInfo = myFrames[myFrameIndex].PreparePresent(hostSyncInfo);
	presentInfo.swapchains.push_back(mySwapchain);

	return presentInfo;
}

template <>
void Swapchain<Vk>::InternalCreateSwapchain(
	const SwapchainConfiguration<Vk>& config, SwapchainHandle<Vk> previous)
{
	ZoneScopedN("Swapchain::InternalCreateSwapchain");

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

	if (previous != nullptr)
	{
#if (GRAPHICS_VALIDATION_LEVEL > 0)
		device.EraseOwnedObjectHandle(getUid(), reinterpret_cast<uint64_t>(previous));
#endif

		vkDestroySwapchainKHR(
			device,
			previous,
			&device.getInstance()->getHostAllocationCallbacks());
	}

#if (GRAPHICS_VALIDATION_LEVEL > 0)
	device.AddOwnedObjectHandle(
		getUid(),
		VK_OBJECT_TYPE_SWAPCHAIN_KHR,
		reinterpret_cast<uint64_t>(mySwapchain),
		std::format("{0}_Swapchain", getName()));
#endif

	uint32_t frameCount = config.imageCount;

	ASSERT(frameCount);

	uint32_t imageCount;
	VK_CHECK(vkGetSwapchainImagesKHR(
		device, mySwapchain, &imageCount, nullptr));

	ASSERT(imageCount == frameCount);

	std::vector<ImageHandle<Vk>> colorImages(imageCount);
	VK_CHECK(vkGetSwapchainImagesKHR(
		device, mySwapchain, &imageCount, colorImages.data()));

	myFrames.clear();
	myFrames.reserve(frameCount);

	for (uint32_t frameIt = 0UL; frameIt < frameCount; frameIt++)
		myFrames.emplace_back(
			getDevice(),
			FrameCreateDesc<Vk>{
				{config.extent,
				 {config.surfaceFormat.format},
				 {VK_IMAGE_LAYOUT_UNDEFINED},
				 {colorImages[frameIt]}},
				frameIt});

	myFrameIndex = frameCount - 1;
}

template <>
Swapchain<Vk>::Swapchain(Swapchain&& other) noexcept
	: DeviceObject(std::forward<Swapchain>(other))
	, myDesc(std::exchange(other.myDesc, {}))
	, mySurface(std::exchange(other.mySurface, {}))
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

	InternalCreateSwapchain(config, previous);
}

template <>
Swapchain<Vk>::~Swapchain()
{
	ZoneScopedN("~Swapchain()");

	if (auto device = getDevice())
	{
		if (mySwapchain != nullptr)
			vkDestroySwapchainKHR(
				*device,
				mySwapchain,
				&device->getInstance()->getHostAllocationCallbacks());

		if (mySurface != nullptr)
			vkDestroySurfaceKHR(
				*device->getInstance(),
				mySurface,
				&device->getInstance()->getHostAllocationCallbacks());
	}
}

template <>
Swapchain<Vk>& Swapchain<Vk>::operator=(Swapchain&& other) noexcept
{
	DeviceObject::operator=(std::forward<Swapchain>(other));
	myDesc = std::exchange(other.myDesc, {});
	mySurface = std::exchange(other.mySurface, {});
	mySwapchain = std::exchange(other.mySwapchain, {});
	myFrames = std::exchange(other.myFrames, {});
	myFrameIndex = std::exchange(other.myFrameIndex, 0);
	return *this;
}

template <>
void Swapchain<Vk>::Swap(Swapchain& rhs) noexcept
{
	DeviceObject::Swap(rhs);
	std::swap(myDesc, rhs.myDesc);
	std::swap(mySurface, rhs.mySurface);
	std::swap(mySwapchain, rhs.mySwapchain);
	std::swap(myFrames, rhs.myFrames);
	std::swap(myFrameIndex, rhs.myFrameIndex);
}
