#include "../swapchain.h"

#include "utils.h"

#include <format>

template <>
RenderInfo<kVk> Swapchain<kVk>::Begin(CommandBufferHandle<kVk> cmd, SubpassContents<kVk> contents, std::span<const VkClearValue> clearValues)
{
	return myFrames[myFrameIndex].Begin(cmd, contents, clearValues);
}

template <>
void Swapchain<kVk>::End(CommandBufferHandle<kVk> cmd)
{
	myFrames[myFrameIndex].End(cmd);
}

template <>
const RenderTargetCreateDesc<kVk>& Swapchain<kVk>::GetRenderTargetDesc() const
{
	return myDesc;
}

template <>
ImageViewHandle<kVk> Swapchain<kVk>::GetColor(uint32_t index) const
{
	return myFrames[myFrameIndex].GetColor(index);
}

template <>
ImageViewHandle<kVk> Swapchain<kVk>::GetDepthStencil() const
{
	return myFrames[myFrameIndex].GetDepthStencil();
}

template <>
ImageLayout<kVk> Swapchain<kVk>::GetColorLayout(uint32_t index) const
{
	return myFrames[myFrameIndex].GetColorLayout(index);
}

template <>
ImageLayout<kVk> Swapchain<kVk>::GetDepthStencilLayout() const
{
	return myFrames[myFrameIndex].GetDepthStencilLayout();
}

template <>
void Swapchain<kVk>::Blit(
	CommandBufferHandle<kVk> cmd,
	const IRenderTarget<kVk>& srcRenderTarget,
	const ImageSubresourceLayers<kVk>& srcSubresource,
	uint32_t srcIndex,
	const ImageSubresourceLayers<kVk>& dstSubresource,
	uint32_t dstIndex,
	Filter<kVk> filter)
{
	myFrames[myFrameIndex].Blit(
		cmd, srcRenderTarget, srcSubresource, srcIndex, dstSubresource, dstIndex, filter);
}

template <>
void Swapchain<kVk>::ClearSingleAttachment(
	CommandBufferHandle<kVk> cmd, const ClearAttachment<kVk>& clearAttachment) const
{
	myFrames[myFrameIndex].ClearSingleAttachment(cmd, clearAttachment);
}

template <>
void Swapchain<kVk>::ClearAllAttachments(
	CommandBufferHandle<kVk> cmd,
	const ClearColorValue<kVk>& color,
	const ClearDepthStencilValue<kVk>& depthStencil) const
{
	myFrames[myFrameIndex].ClearAllAttachments(cmd, color, depthStencil);
}

template <>
void Swapchain<kVk>::ClearColor(
	CommandBufferHandle<kVk> cmd, const ClearColorValue<kVk>& color, uint32_t index)
{
	myFrames[myFrameIndex].ClearColor(cmd, color, index);
}

template <>
void Swapchain<kVk>::ClearDepthStencil(
	CommandBufferHandle<kVk> cmd, const ClearDepthStencilValue<kVk>& depthStencil)
{
	myFrames[myFrameIndex].ClearDepthStencil(cmd, depthStencil);
}

template <>
void Swapchain<kVk>::TransitionColor(
	CommandBufferHandle<kVk> cmd, ImageLayout<kVk> layout, uint32_t index)
{
	myFrames[myFrameIndex].TransitionColor(cmd, layout, index);
}

template <>
void Swapchain<kVk>::TransitionDepthStencil(CommandBufferHandle<kVk> cmd, ImageLayout<kVk> layout)
{
	myFrames[myFrameIndex].TransitionDepthStencil(cmd, layout);
}

template <GraphicsApi G>
AttachmentLoadOp<G> Swapchain<G>::GetColorLoadOp(uint32_t index) const
{
	return myFrames[myFrameIndex].GetColorLoadOp(index);
}

template <GraphicsApi G>
AttachmentStoreOp<G> Swapchain<G>::GetColorStoreOp(uint32_t index) const
{
	return myFrames[myFrameIndex].GetColorStoreOp(index);
}

template <GraphicsApi G>
AttachmentLoadOp<G> Swapchain<G>::GetDepthStencilLoadOp() const
{
	return myFrames[myFrameIndex].GetDepthStencilLoadOp();
}

template <GraphicsApi G>
AttachmentStoreOp<G> Swapchain<G>::GetDepthStencilStoreOp() const
{
	return myFrames[myFrameIndex].GetDepthStencilStoreOp();
}

template <>
void Swapchain<kVk>::SetColorLoadOp(uint32_t index, AttachmentLoadOp<kVk> loadOp)
{
	myFrames[myFrameIndex].SetColorLoadOp(index, loadOp);
}

template <>
void Swapchain<kVk>::SetColorStoreOp(uint32_t index, AttachmentStoreOp<kVk> storeOp)
{
	myFrames[myFrameIndex].SetColorStoreOp(index, storeOp);
}

template <>
void Swapchain<kVk>::SetDepthStencilLoadOp(AttachmentLoadOp<kVk> loadOp)
{
	myFrames[myFrameIndex].SetDepthStencilLoadOp(loadOp);
}

template <>
void Swapchain<kVk>::SetDepthStencilStoreOp(AttachmentStoreOp<kVk> storeOp)
{
	myFrames[myFrameIndex].SetDepthStencilStoreOp(storeOp);
}

template <>
std::tuple<bool, uint32_t, uint32_t> Swapchain<kVk>::Flip()
{
	ZoneScoped;

	auto& device = *InternalGetDevice();

	auto lastFrameIndex = myFrameIndex;
	
	Fence<kVk> fence(InternalGetDevice(), FenceCreateDesc<kVk>{});

	auto frameIndex = myFrameIndex.load();
	auto flipResult = CheckFlipOrPresentResult(vkAcquireNextImageKHR(
		device,
		mySwapchain,
		UINT64_MAX,
		VK_NULL_HANDLE,
		fence,
		&frameIndex));
	myFrameIndex.store(frameIndex);

	auto& newFrame = myFrames[myFrameIndex];

	newFrame.GetFence().Swap(fence);

	auto zoneNameStr =
		std::format("Swapchain::flip frame:{0}", flipResult == VK_SUCCESS ? myFrameIndex : ~0U);

	ZoneName(zoneNameStr.c_str(), zoneNameStr.size());

	return std::make_tuple(flipResult == VK_SUCCESS, lastFrameIndex, myFrameIndex);
}

template <>
QueuePresentInfo<kVk> Swapchain<kVk>::PreparePresent(const QueueHostSyncInfo<kVk>& hostSyncInfo)
{
	ZoneScopedN("Swapchain::PreparePresent");

	auto presentInfo = myFrames[myFrameIndex].PreparePresent(hostSyncInfo);
	presentInfo.swapchains.push_back(mySwapchain);

	return presentInfo;
}

template <>
void Swapchain<kVk>::InternalCreateSwapchain(
	const SwapchainConfiguration<kVk>& config, SwapchainHandle<kVk> previous)
{
	ZoneScopedN("Swapchain::InternalCreateSwapchain");

	auto& device = *InternalGetDevice();

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
		&device.GetInstance()->GetHostAllocationCallbacks(),
		&mySwapchain));

	if (previous != nullptr)
	{
#if (GRAPHICS_VALIDATION_LEVEL > 0)
		device.EraseOwnedObjectHandle(GetUid(), reinterpret_cast<uint64_t>(previous));
#endif

		vkDestroySwapchainKHR(
			device,
			previous,
			&device.GetInstance()->GetHostAllocationCallbacks());
	}

#if (GRAPHICS_VALIDATION_LEVEL > 0)
	device.AddOwnedObjectHandle(
		GetUid(),
		VK_OBJECT_TYPE_SWAPCHAIN_KHR,
		reinterpret_cast<uint64_t>(mySwapchain),
		std::format("{0}_Swapchain", GetName()));
#endif

	uint32_t frameCount = config.imageCount;

	ASSERT(frameCount);

	uint32_t imageCount;
	VK_CHECK(vkGetSwapchainImagesKHR(
		device, mySwapchain, &imageCount, nullptr));

	ASSERT(imageCount == frameCount);

	std::vector<ImageHandle<kVk>> colorImages(imageCount);
	VK_CHECK(vkGetSwapchainImagesKHR(
		device, mySwapchain, &imageCount, colorImages.data()));

	myFrames.clear();
	myFrames.reserve(frameCount);

	for (uint32_t frameIt = 0UL; frameIt < frameCount; frameIt++)
		myFrames.emplace_back(
			InternalGetDevice(),
			FrameCreateDesc<kVk>{
				{config.extent,
				 {config.surfaceFormat.format},
				 {VK_IMAGE_LAYOUT_UNDEFINED},
				 {colorImages[frameIt]}},
				frameIt});

	myFrameIndex.store(frameCount - 1);
}

template <>
Swapchain<kVk>::Swapchain(Swapchain&& other) noexcept
	: DeviceObject(std::forward<Swapchain>(other))
	, myDesc(other.myDesc)
	, mySurface(std::exchange(other.mySurface, {}))
	, mySwapchain(std::exchange(other.mySwapchain, {}))
	, myFrames(std::exchange(other.myFrames, {}))
	, myFrameIndex(std::exchange(other.myFrameIndex, {}))
{}

template <>
Swapchain<kVk>::Swapchain(
	const std::shared_ptr<Device<kVk>>& device,
	const SwapchainConfiguration<kVk>& config,
	SurfaceHandle<kVk>&& surface,
	SwapchainHandle<kVk> previous)
	: DeviceObject(device, {})
	, myDesc{config.extent} // more?
	, mySurface(std::forward<SurfaceHandle<kVk>>(surface))
{
	ZoneScopedN("Swapchain()");

	InternalCreateSwapchain(config, previous);
}

template <>
Swapchain<kVk>::~Swapchain()
{
	ZoneScopedN("~Swapchain()");

	if (auto device = InternalGetDevice())
	{
		if (mySwapchain != nullptr)
			vkDestroySwapchainKHR(
				*device,
				mySwapchain,
				&device->GetInstance()->GetHostAllocationCallbacks());

		if (mySurface != nullptr)
			vkDestroySurfaceKHR(
				*device->GetInstance(),
				mySurface,
				&device->GetInstance()->GetHostAllocationCallbacks());
	}
}

template <>
Swapchain<kVk>& Swapchain<kVk>::operator=(Swapchain&& other) noexcept
{
	DeviceObject::operator=(std::forward<Swapchain>(other));
	myDesc = std::exchange(other.myDesc, {});
	mySurface = std::exchange(other.mySurface, {});
	mySwapchain = std::exchange(other.mySwapchain, {});
	myFrames = std::exchange(other.myFrames, {});
	myFrameIndex = std::exchange(other.myFrameIndex, {});
	return *this;
}

template <>
void Swapchain<kVk>::Swap(Swapchain& rhs) noexcept
{
	DeviceObject::Swap(rhs);
	std::swap(myDesc, rhs.myDesc);
	std::swap(mySurface, rhs.mySurface);
	std::swap(mySwapchain, rhs.mySwapchain);
	std::swap(myFrames, rhs.myFrames);
	std::swap(myFrameIndex, rhs.myFrameIndex);
}
