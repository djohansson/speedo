#include <rhi/capi.h>
#include <rhi/device.h>
#include <rhi/rhiapplication.h>
#include <rhi/swapchain.h>
#include <rhi/vulkan/utils.h>

#include <algorithm>
#include <format>
#include <limits>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(Swapchain<kVk>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(Swapchain<kVk>);

template <>
const RenderTargetBeginInfo<kVk>& Swapchain<kVk>::Begin(CommandBufferHandle<kVk> cmd, SubpassContents<kVk> contents)
{
	return myFrames[myFrameIndex].Begin(cmd, contents);
}

template <>
void Swapchain<kVk>::End(CommandBufferHandle<kVk> cmd)
{
	myFrames[myFrameIndex].End(cmd);
}

template <>
RenderTargetPassHandle<kVk> Swapchain<kVk>::GetHandle()
{
	return myFrames[myFrameIndex].GetHandle();
}
	
template <>
Extent2d<kVk> Swapchain<kVk>::GetExtent() const
{
	return myFrames[myFrameIndex].GetExtent();
}

template <>
std::span<const ImageHandle<kVk>> Swapchain<kVk>::GetImages() const
{
	return myFrames[myFrameIndex].GetImages();
}

template <>
std::span<const ImageViewHandle<kVk>> Swapchain<kVk>::GetAttachments() const
{
	return myFrames[myFrameIndex].GetAttachments();
}

template <>
std::span<const AttachmentDescription<kVk>> Swapchain<kVk>::GetAttachmentDescs() const
{
	return myFrames[myFrameIndex].GetAttachmentDescs();
}

template <>
ImageLayout<kVk> Swapchain<kVk>::GetLayout(uint32_t index) const
{
	return myFrames[myFrameIndex].GetLayout(index);
}

template <>
const std::optional<PipelineRenderingCreateInfo<kVk>>& Swapchain<kVk>::GetPipelineRenderingCreateInfo() const 
{
	static constexpr std::optional<PipelineRenderingCreateInfo<kVk>> kNullPipelineRenderingCreateInfo;

	return kNullPipelineRenderingCreateInfo;
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
void Swapchain<kVk>::Copy(
	CommandBufferHandle<kVk> cmd,
	const IRenderTarget<kVk>& srcRenderTarget,
	const ImageSubresourceLayers<kVk>& srcSubresource,
	uint32_t srcIndex,
	const ImageSubresourceLayers<kVk>& dstSubresource,
	uint32_t dstIndex)
{
	myFrames[myFrameIndex].Copy(cmd, srcRenderTarget, srcSubresource, srcIndex, dstSubresource, dstIndex);
}

template <>
void Swapchain<kVk>::ClearAll(
	CommandBufferHandle<kVk> cmd,
	std::span<const ClearValue<kVk>> values) const
{
	myFrames[myFrameIndex].ClearAll(cmd, values);
}

template <>
void Swapchain<kVk>::Clear(
	CommandBufferHandle<kVk> cmd, const ClearValue<kVk>& value, uint32_t index)
{
	myFrames[myFrameIndex].Clear(cmd, value, index);
}

template <>
void Swapchain<kVk>::Transition(
	CommandBufferHandle<kVk> cmd, ImageLayout<kVk> layout, ImageAspectFlags<kVk> aspectFlags, uint32_t index)
{
	myFrames[myFrameIndex].Transition(cmd, layout, aspectFlags, index);
}

template <>
void Swapchain<kVk>::SetLoadOp(AttachmentLoadOp<kVk> loadOp, uint32_t index, AttachmentLoadOp<kVk> stencilLoadOp)
{
	myFrames[myFrameIndex].SetLoadOp(loadOp, index, stencilLoadOp);
}

template <>
void Swapchain<kVk>::SetStoreOp(AttachmentStoreOp<kVk> storeOp, uint32_t index, AttachmentStoreOp<kVk> stencilStoreOp)
{
	myFrames[myFrameIndex].SetStoreOp(storeOp, index, stencilStoreOp);
}

template <>
void Swapchain<kVk>::OnPresentResult(Result<kVk> result) noexcept
{
	if (result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR)
		myNeedsRecreate = true;
}

template <>
FlipResult<kVk> Swapchain<kVk>::Flip()
{
	ZoneScoped;

	auto lastFrameIndex = myFrameIndex;

	// drop acquire fences that have signaled since, so the list stays short
	std::erase_if(myAcquireFences, [](const Fence<kVk>& acquireFence) { return acquireFence.Wait(0ULL); });

	Fence<kVk> fence(FenceCreateDesc<kVk>{SuperType::CreateDeviceObjectCreateDesc("acquireNextImageFence")});
	Semaphore<kVk> semaphore(SemaphoreCreateDesc<kVk>{SuperType::CreateDeviceObjectCreateDesc("acquireNextImageSemaphore"), VK_SEMAPHORE_TYPE_BINARY});

	auto flipResult = vkAcquireNextImageKHR(
		GetDevice(),
		mySwapchain,
		UINT64_MAX,
		semaphore,
		fence,
		&myFrameIndex);

	// suboptimal still acquires an image (and signals the fence and semaphore). out of date is expected while
	// resizing: the frame is skipped, and the swapchain recreated before the next one (see NeedsRecreate).
	bool success = flipResult == VK_SUCCESS || flipResult == VK_SUBOPTIMAL_KHR;
	OnPresentResult(flipResult);
	if (success)
		myAcquireFences.emplace_back(std::move(fence));
	else if (flipResult == VK_ERROR_OUT_OF_DATE_KHR)
		myFrameIndex = lastFrameIndex; // image index is undefined on failure
	else
		VK_CHECK(flipResult);

	auto zoneNameStr =
		std::format("Swapchain::flip frame:{}", success ? myFrameIndex : ~0U);

	ZoneName(zoneNameStr.c_str(), zoneNameStr.size());

	return FlipResult<kVk>{
		.acquireNextImageSemaphore = std::move(semaphore),
		.lastFrameIndex = lastFrameIndex,
		.newFrameIndex = myFrameIndex,
		.success = success};
}

template <>
QueuePresentInfo<kVk> Swapchain<kVk>::PreparePresent()
{
	ZoneScopedN("Swapchain::PreparePresent");

	auto presentInfo = myFrames[myFrameIndex].PreparePresent();
	
	presentInfo.swapchains.push_back(mySwapchain);

	static bool gSupportsPresentId = SupportsExtension(VK_KHR_PRESENT_ID_EXTENSION_NAME, GetInstance());
	static uint64_t gPresentId = 0ULL;
	if (gSupportsPresentId)
	{
		presentInfo.presentIds.resize(presentInfo.swapchains.size());
		for (size_t i = 0; i < presentInfo.swapchains.size(); ++i)
			presentInfo.presentIds[i] = gPresentId++;
	}

	return presentInfo;
}

template <>
bool Swapchain<kVk>::WaitPresent(uint64_t presentId, uint64_t timeout) const
{
	ZoneScopedN("Swapchain::WaitPresent");

	ENSURE(gVkWaitForPresentKHR != nullptr);

	auto result = gVkWaitForPresentKHR(GetDevice(), mySwapchain, presentId, timeout);
	
	if (result == VK_TIMEOUT)
		return false;

	// the present was dropped, or the swapchain recreated, while resizing: nothing left to wait for
	if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
		return true;

	VK_CHECK(result);

	return true;
}

template <>
void Swapchain<kVk>::CreateSwapchain()
{
	ZoneScopedN("Swapchain::CreateSwapchain");

	auto& device = GetDevice();
	auto previous = static_cast<SwapchainHandle<kVk>>(mySwapchain);

	// the surface size has changed when we get here from a resize, so query it before creating the swapchain.
	// a currentExtent of 0xFFFFFFFF means the surface size is determined by the swapchain: keep the current extent.
	GetInstance().UpdateSurfaceCapabilities(device.GetPhysicalDevice(), GetSurface());
	const auto& capabilities = GetInstance().GetSwapchainInfo(device.GetPhysicalDevice(), GetSurface()).capabilities;
	if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max())
		InternalGetDesc().extent = capabilities.currentExtent;

	VkSwapchainCreateInfoKHR info{.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
	info.surface = mySurface;
	info.minImageCount = GetDesc().images.size();
	info.imageFormat = GetDesc().surfaceFormat.format;
	info.imageColorSpace = GetDesc().surfaceFormat.colorSpace;
	info.imageExtent = GetDesc().extent;
	info.imageArrayLayers = 1;
	info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	info.presentMode = GetDesc().presentMode;
	info.clipped = VK_TRUE;
	info.oldSwapchain = previous;

	VK_CHECK(vkCreateSwapchainKHR(
		device,
		&info,
		&GetInstance().GetHostAllocationCallbacks(),
		&mySwapchain));

	if (previous != VK_NULL_HANDLE)
	{
		// acquire fences reference the previous swapchain, so they must be waited on before it is destroyed
		for (const auto& acquireFence : myAcquireFences)
			acquireFence.Wait();
		myAcquireFences.clear();

#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
		EraseOwnedObjectHandle<kVk>(GetDesc().uuid, reinterpret_cast<uint64_t>(previous));
#endif
		vkDestroySwapchainKHR(
			device,
			previous,
			&GetInstance().GetHostAllocationCallbacks());
	}

#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
	AddOwnedObjectHandle<kVk>(
		device,
		GetDesc().uuid,
		VK_OBJECT_TYPE_SWAPCHAIN_KHR,
		reinterpret_cast<uint64_t>(mySwapchain),
		std::format("{}_Swapchain", uuids::to_string(GetDesc().uuid)));
#endif

	uint32_t frameCount = GetDesc().images.size();

	ENSURE(frameCount);

	uint32_t imageCount;
	VK_CHECK(vkGetSwapchainImagesKHR(
		device, mySwapchain, &imageCount, nullptr));

	ENSURE(imageCount == frameCount);

	std::vector<ImageHandle<kVk>> colorImages(imageCount);
	VK_CHECK(vkGetSwapchainImagesKHR(
		device, mySwapchain, &imageCount, colorImages.data()));

	myFrames.clear();
	myFrames.reserve(frameCount);

	for (uint32_t frameIt = 0UL; frameIt < frameCount; frameIt++)
		myFrames.emplace_back(
			FrameCreateDesc<kVk>
			{
				RenderTargetCreateDesc<kVk>
				{
					SuperType::CreateDeviceObjectCreateDesc(std::format("Frame{}", frameIt)),
					GetDesc().extent,
				 	{GetDesc().surfaceFormat.format},
					{VK_IMAGE_LAYOUT_UNDEFINED},
					{VK_IMAGE_ASPECT_COLOR_BIT},
					{colorImages[frameIt]},
					{ClearValue<kVk>{}},
					1,
					GetDesc().useDynamicRendering
				},
				frameIt
			});

	myFrameIndex = frameCount - 1;
	myNeedsRecreate = false;
}

template <>
void Swapchain<kVk>::Swap(Swapchain& rhs) noexcept
{
	DeviceObject<Swapchain<kVk>>::Swap(rhs);
	std::swap(mySurface, rhs.mySurface);
	std::swap(mySwapchain, rhs.mySwapchain);
	std::swap(myFrames, rhs.myFrames);
	std::swap(myFrameIndex, rhs.myFrameIndex);
	std::swap(myAcquireFences, rhs.myAcquireFences);
	std::swap(myNeedsRecreate, rhs.myNeedsRecreate);
}

template <>
Swapchain<kVk>::Swapchain(Swapchain&& other) noexcept
{
	Swap(other);
}

template <>
Swapchain<kVk>::Swapchain(Swapchain<kVk>::CreateDescType&& desc)
	: DeviceObject<Swapchain<kVk>>(std::forward<Swapchain<kVk>::CreateDescType>(desc))
	, mySurface(GetDesc().surface)
{
	ZoneScopedN("Swapchain()");

	CreateSwapchain();
}

template <>
Swapchain<kVk>::~Swapchain()
{
	ZoneScopedN("~Swapchain()");

	if (IsValid())
	{
		for (const auto& acquireFence : myAcquireFences)
			acquireFence.Wait();
		myAcquireFences.clear();

		if (mySwapchain != nullptr)
			vkDestroySwapchainKHR(
				GetDevice(),
				mySwapchain,
				&GetInstance().GetHostAllocationCallbacks());

		if (mySurface != nullptr)
			vkDestroySurfaceKHR(
				GetInstance(),
				mySurface,
				&GetInstance().GetHostAllocationCallbacks());
	}
}

template <>
Swapchain<kVk>& Swapchain<kVk>::operator=(Swapchain&& other) noexcept
{
	Swap(other);
	return *this;
}

} // namespace rhi
