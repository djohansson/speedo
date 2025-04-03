#include "../swapchain.h"

#include "utils.h"

#include <format>

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
const RenderTargetCreateDesc<kVk>& Swapchain<kVk>::GetRenderTargetDesc() const
{
	return myDesc;
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
FlipResult<kVk> Swapchain<kVk>::Flip()
{
	ZoneScoped;

	auto lastFrameIndex = myFrameIndex;
	
	Fence<kVk> fence(InternalGetDevice(), FenceCreateDesc<kVk>{});
	Semaphore<kVk> semaphore(InternalGetDevice(), SemaphoreCreateDesc<kVk>{.type = VK_SEMAPHORE_TYPE_BINARY});

	auto frameIndex = myFrameIndex.load();
	auto flipResult = CheckFlipOrPresentResult(vkAcquireNextImageKHR(
		*InternalGetDevice(),
		mySwapchain,
		UINT64_MAX,
		semaphore,
		fence,
		&frameIndex));
	myFrameIndex.store(frameIndex);

	auto& lastFrame = myFrames[lastFrameIndex];
	auto& newFrame = myFrames[myFrameIndex];

	newFrame.GetFence().Swap(fence);

	auto zoneNameStr =
		std::format("Swapchain::flip frame:{0}", flipResult == VK_SUCCESS ? myFrameIndex : ~0U);

	ZoneName(zoneNameStr.c_str(), zoneNameStr.size());

	return FlipResult<kVk>{
		.acquireNextImageSemaphore = std::move(semaphore),
		.lastFrameIndex = lastFrameIndex,
		.newFrameIndex = myFrameIndex,
		.succeess = flipResult == VK_SUCCESS};
}

template <>
QueuePresentInfo<kVk> Swapchain<kVk>::PreparePresent()
{
	ZoneScopedN("Swapchain::PreparePresent");

	auto presentInfo = myFrames[myFrameIndex].PreparePresent();
	
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
	info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	info.presentMode = config.presentMode;
	info.clipped = VK_TRUE;
	info.oldSwapchain = previous;

	VK_ENSURE(vkCreateSwapchainKHR(
		device,
		&info,
		&device.GetInstance()->GetHostAllocationCallbacks(),
		&mySwapchain));

	if (previous != nullptr)
	{
#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
		device.EraseOwnedObjectHandle(GetUuid(), reinterpret_cast<uint64_t>(previous));
#endif

		vkDestroySwapchainKHR(
			device,
			previous,
			&device.GetInstance()->GetHostAllocationCallbacks());
	}

#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
	device.AddOwnedObjectHandle(
		GetUuid(),
		VK_OBJECT_TYPE_SWAPCHAIN_KHR,
		reinterpret_cast<uint64_t>(mySwapchain),
		std::format("{}_Swapchain", GetName()));
#endif

	uint32_t frameCount = config.imageCount;

	ASSERT(frameCount);

	uint32_t imageCount;
	VK_ENSURE(vkGetSwapchainImagesKHR(
		device, mySwapchain, &imageCount, nullptr));

	ASSERT(imageCount == frameCount);

	std::vector<ImageHandle<kVk>> colorImages(imageCount);
	VK_ENSURE(vkGetSwapchainImagesKHR(
		device, mySwapchain, &imageCount, colorImages.data()));

	myFrames.clear();
	myFrames.reserve(frameCount);

	for (uint32_t frameIt = 0UL; frameIt < frameCount; frameIt++)
		myFrames.emplace_back(
			InternalGetDevice(),
			FrameCreateDesc<kVk>{
				{.extent = config.extent,
				 .imageFormats = {config.surfaceFormat.format},
				 .imageLayouts = {VK_IMAGE_LAYOUT_UNDEFINED},
				 .imageAspectFlags = {VK_IMAGE_ASPECT_COLOR_BIT},
				 .images = {colorImages[frameIt]},
				 .clearValues = {ClearValue<kVk>{}},
				 .layerCount = 1,
				 .useDynamicRendering = config.useDynamicRendering},
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
	: DeviceObject(device, {}, uuids::uuid_system_generator{}())
	, myDesc{.extent = config.extent} // more?
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
