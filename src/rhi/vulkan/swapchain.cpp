#include <rhi/capi.h>
#include <rhi/device.h>
#include <rhi/rhi.h>
#include <rhi/swapchain.h>
#include <rhi/vulkan/convert.h>
#include <rhi/vulkan/utils.h>

#include <algorithm>
#include <format>
#include <limits>
#include <print>

#if defined(_WIN32)
// VK_EXT_full_screen_exclusive's types (vulkan_win32.h names windows.h's): only the kernel part
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	ifndef NOGDI
#		define NOGDI
#	endif
#	ifndef NOUSER
#		define NOUSER
#	endif
#	include <windows.h>
#	include <vulkan/vulkan_win32.h>
#endif

namespace rhi
{

namespace swapchain
{

#if defined(_WIN32)
struct FullScreenExclusive
{
	PFN_vkAcquireFullScreenExclusiveModeEXT acquire = nullptr;
	PFN_vkReleaseFullScreenExclusiveModeEXT release = nullptr;
};

// the extension's functions, if the device enabled it (see Device)
const FullScreenExclusive& GetFullScreenExclusive(VkDevice device, VkPhysicalDevice physicalDevice)
{
	static const FullScreenExclusive gFunctions = [device, physicalDevice]
	{
		FullScreenExclusive functions;
		if (SupportsExtension(VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME, physicalDevice))
		{
			functions.acquire = reinterpret_cast<PFN_vkAcquireFullScreenExclusiveModeEXT>(
				vkGetDeviceProcAddr(device, "vkAcquireFullScreenExclusiveModeEXT"));
			functions.release = reinterpret_cast<PFN_vkReleaseFullScreenExclusiveModeEXT>(
				vkGetDeviceProcAddr(device, "vkReleaseFullScreenExclusiveModeEXT"));
		}
		return functions;
	}();
	return gFunctions;
}
#endif

} // namespace swapchain

IMPLEMENT_OBJECT_GETINSTANCE(Swapchain<kVk>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(Swapchain<kVk>);

template <>
const RenderTargetBeginInfo<kVk>& Swapchain<kVk>::Begin(CommandBufferHandle<kVk> cmd, SubpassContents contents)
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
Extent2d Swapchain<kVk>::GetExtent() const
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
ImageLayout Swapchain<kVk>::GetLayout(uint32_t index) const
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
	Filter filter)
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
	std::span<const ClearValue> values) const
{
	myFrames[myFrameIndex].ClearAll(cmd, values);
}

template <>
void Swapchain<kVk>::Clear(
	CommandBufferHandle<kVk> cmd, const ClearValue& value, uint32_t index)
{
	myFrames[myFrameIndex].Clear(cmd, value, index);
}

template <>
void Swapchain<kVk>::Transition(
	CommandBufferHandle<kVk> cmd, ImageLayout layout, ImageAspect aspectFlags, uint32_t index)
{
	myFrames[myFrameIndex].Transition(cmd, layout, aspectFlags, index);
}

template <>
void Swapchain<kVk>::SetLoadOp(LoadOp loadOp, uint32_t index, LoadOp stencilLoadOp)
{
	myFrames[myFrameIndex].SetLoadOp(loadOp, index, stencilLoadOp);
}

template <>
void Swapchain<kVk>::SetStoreOp(StoreOp storeOp, uint32_t index, StoreOp stencilStoreOp)
{
	myFrames[myFrameIndex].SetStoreOp(storeOp, index, stencilStoreOp);
}

template <>
void Swapchain<kVk>::SetClearValue(const ClearValue& value, uint32_t index)
{
	myFrames[myFrameIndex].SetClearValue(value, index);
}

template <>
void Swapchain<kVk>::OnPresentResult(PresentResult result) noexcept
{
	if (result == PresentResult::kSuboptimal || result == PresentResult::kOutOfDate)
		myNeedsRecreate = true;
}

template <>
Extent2d Swapchain<kVk>::QuerySurfaceExtent()
{
	auto& instance = GetInstance();
	auto physicalDevice = GetDevice().GetPhysicalDevice();
	instance.UpdateSurfaceCapabilities(physicalDevice, mySurface);
	auto extent = instance.GetSwapchainInfo(physicalDevice, mySurface).capabilities.currentExtent;
	if (extent.width == std::numeric_limits<uint32_t>::max()) // surface size is determined by the swapchain
		return GetDesc().extent;
	return vk::FromVk(extent);
}

template <>
FlipResult<kVk> Swapchain<kVk>::Flip()
{
	ZoneScoped;

	auto lastFrameIndex = myFrameIndex;

	// drop acquire fences that have signaled since, so the list stays short
	std::erase_if(myAcquireFences, [](const Fence<kVk>& acquireFence) { return acquireFence.Wait(0ULL); });

	Fence<kVk> fence(FenceCreateDesc<kVk>{SuperType::CreateDeviceObjectCreateDesc("acquireNextImageFence")});
	Semaphore<kVk> semaphore(SemaphoreCreateDesc<kVk>{SuperType::CreateDeviceObjectCreateDesc("acquireNextImageSemaphore"), SemaphoreType::kBinary});

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
	OnPresentResult(vk::ToPresentResult(flipResult));
	if (success)
		myAcquireFences.emplace_back(std::move(fence));
	else if (flipResult == VK_ERROR_OUT_OF_DATE_KHR || flipResult == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT)
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
	myHasPresented = true;

	// from 1: a present id of 0 is none
	// a device extension (it was looked for among the instance's, so presents never had ids)
	static bool gSupportsPresentId = SupportsExtension(VK_KHR_PRESENT_ID_EXTENSION_NAME, GetDevice().GetPhysicalDevice());
	static uint64_t gPresentId = 1ULL;
	if (gSupportsPresentId)
	{
		presentInfo.presentIds.resize(presentInfo.swapchains.size());
		for (size_t i = 0; i < presentInfo.swapchains.size(); ++i)
			presentInfo.presentIds[i] = gPresentId++;
		myLastPresentId = presentInfo.presentIds.back();
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
bool Swapchain<kVk>::WaitPresented(uint64_t timeout) const
{
	ZoneScopedN("Swapchain::WaitPresented");

	// none presented since it was made (or it was released, see Release: nothing left to show)
	if (mySwapchain == nullptr || myLastPresentId == 0 || gVkWaitForPresentKHR == nullptr)
		return false;

	return WaitPresent(myLastPresentId, timeout);
}

template <>
std::pair<uint32_t, uint32_t> Swapchain<kVk>::GetImageCountLimits() const
{
	const auto& capabilities = GetInstance().GetSwapchainInfo(GetDevice().GetPhysicalDevice(), GetSurface()).capabilities;
	auto maxImageCount = capabilities.maxImageCount;
	if (GetDesc().maxImageCount != 0)
		maxImageCount = maxImageCount != 0 ? std::min(maxImageCount, GetDesc().maxImageCount) : GetDesc().maxImageCount;
	return {capabilities.minImageCount, std::max(maxImageCount, maxImageCount != 0 ? capabilities.minImageCount : 0U)};
}

template <>
void Swapchain<kVk>::Configure(PresentationMode mode, uint32_t imageCount)
{
	const auto& info = GetInstance().GetSwapchainInfo(GetDevice().GetPhysicalDevice(), GetSurface());
	if (std::ranges::find(info.presentModes, vk::ToVk(mode)) != info.presentModes.end())
		InternalGetDesc().presentMode = vk::ToVk(mode);
	auto [minImageCount, maxImageCount] = GetImageCountLimits();
	imageCount = std::max(imageCount, minImageCount);
	if (maxImageCount != 0)
		imageCount = std::min(imageCount, maxImageCount);
	InternalGetDesc().images.resize(imageCount); // their handles: CreateSwapchain gets them
	myNeedsRecreate = true;
}

template <>
PresentationMode Swapchain<kVk>::GetPresentationMode() const
{
	return vk::FromVk(GetDesc().presentMode);
}

template <>
uint32_t Swapchain<kVk>::GetPresentationModes() const
{
	uint32_t modes = 0;
	for (auto mode : GetInstance().GetSwapchainInfo(GetDevice().GetPhysicalDevice(), GetSurface()).presentModes)
		if (auto presentation = vk::FromVk(mode); vk::ToVk(presentation) == mode)
			modes |= 1U << static_cast<uint32_t>(presentation);
	return modes;
}

template <>
void Swapchain<kVk>::SetExclusiveFullscreen(WindowHandle window) noexcept
{
	if (window == myExclusiveWindow)
		return;
	myExclusiveWindow = window;
	myNeedsRecreate = true;
}

template <>
void Swapchain<kVk>::InternalDestroySwapchain(SwapchainHandle<kVk> swapchain)
{
#if defined(_WIN32)
	if (myExclusiveAcquired)
	{
		const auto& functions = swapchain::GetFullScreenExclusive(GetDevice(), GetDevice().GetPhysicalDevice());
		functions.release(GetDevice(), swapchain);
		myExclusiveAcquired = false;
	}
#endif
	Untrack(VK_OBJECT_TYPE_SWAPCHAIN_KHR, swapchain);
	vkDestroySwapchainKHR(GetDevice(), swapchain, &GetInstance().GetHostAllocationCallbacks());
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
		InternalGetDesc().extent = vk::FromVk(capabilities.currentExtent);

#if defined(_WIN32)
	// released while previous is still current: it is retired once the new one is made from it, and a retired swapchain's
	// exclusive mode mustn't be released (VUID-vkReleaseFullScreenExclusiveModeEXT-swapchain-02677)
	if (myExclusiveAcquired && previous != VK_NULL_HANDLE)
	{
		swapchain::GetFullScreenExclusive(device, device.GetPhysicalDevice()).release(device, previous);
		myExclusiveAcquired = false;
	}
#endif

	VkSwapchainCreateInfoKHR info{.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
	info.surface = mySurface;
	info.minImageCount = GetDesc().images.size();
	info.imageFormat = GetDesc().surfaceFormat.format;
	info.imageColorSpace = GetDesc().surfaceFormat.colorSpace;
	info.imageExtent = vk::ToVk(GetDesc().extent);
	info.imageArrayLayers = 1;
	// transfer source: a frame read back (SPEEDO_CAPTURE)
	info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
					  VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	// drawn by the graphics family, presented by another's: shared by both, rather than owned by one and transferred
	// (presenting another family's exclusive image without a transfer is invalid, though it drew)
	std::array<uint32_t, 2> queueFamilies{GetDesc().graphicsQueueFamily, GetDesc().presentQueueFamily};
	if (queueFamilies[0] != queueFamilies[1])
	{
		info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
		info.queueFamilyIndexCount = static_cast<uint32_t>(queueFamilies.size());
		info.pQueueFamilyIndices = queueFamilies.data();
	}
	else
		info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	info.presentMode = GetDesc().presentMode;
	info.clipped = VK_TRUE;
	info.oldSwapchain = previous;

#if defined(_WIN32)
	// exclusive fullscreen, where the window is in it (see SetExclusiveFullscreen): application controlled, so it is
	// taken (below) only then, and the window's monitor named, as the extension needs for a surface of a window
	const auto& fullScreenExclusive = swapchain::GetFullScreenExclusive(device, device.GetPhysicalDevice());
	VkSurfaceFullScreenExclusiveWin32InfoEXT exclusiveWin32Info{.sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT};
	VkSurfaceFullScreenExclusiveInfoEXT exclusiveInfo{.sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT};
	bool exclusive = myExclusiveWindow != kInvalidWindowHandle && fullScreenExclusive.acquire != nullptr;
	if (exclusive)
	{
		exclusiveWin32Info.hmonitor = static_cast<HMONITOR>(PlatformGetWin32Monitor(myExclusiveWindow));
		exclusiveInfo.pNext = &exclusiveWin32Info;
		exclusiveInfo.fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT;
		info.pNext = &exclusiveInfo;
	}
#endif

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

		InternalDestroySwapchain(previous);
	}

	Track(static_cast<VkDevice>(device), VK_OBJECT_TYPE_SWAPCHAIN_KHR, mySwapchain, GetName());

#if defined(_WIN32)
	// fails where the window hasn't the focus (VK_ERROR_INITIALIZATION_FAILED): it presents composited then, and a lost
	// exclusive mode (a present's VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) recreates it, which tries again
	if (exclusive)
	{
		auto result = fullScreenExclusive.acquire(device, mySwapchain);
		myExclusiveAcquired = result == VK_SUCCESS;
		std::println("swapchain: exclusive fullscreen {}", myExclusiveAcquired ? "acquired" : std::format("not acquired ({})", static_cast<int>(result)));
	}
#endif

	// minImageCount is a minimum: the driver may make more, and the frames are the images it made
	uint32_t imageCount;
	VK_CHECK(vkGetSwapchainImagesKHR(
		device, mySwapchain, &imageCount, nullptr));

	std::vector<ImageHandle<kVk>> colorImages(imageCount);
	VK_CHECK(vkGetSwapchainImagesKHR(
		device, mySwapchain, &imageCount, colorImages.data()));

	ENSURE(imageCount);
	if (imageCount != GetDesc().images.size())
		std::println(
			"swapchain: {} images asked for, the driver made {}", GetDesc().images.size(), imageCount);
	InternalGetDesc().images.resize(imageCount);
	uint32_t frameCount = imageCount;

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
					{vk::FromVk(GetDesc().surfaceFormat.format)},
					{ImageLayout::kUndefined},
					{ImageAspect::kColor},
					{colorImages[frameIt]},
					{ClearValue{}},
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
	std::swap(myLastPresentId, rhs.myLastPresentId);
	std::swap(myHasPresented, rhs.myHasPresented);
	std::swap(myExclusiveWindow, rhs.myExclusiveWindow);
	std::swap(myExclusiveAcquired, rhs.myExclusiveAcquired);
}

template <>
void Swapchain<kVk>::Release()
{
	ZoneScopedN("Swapchain::Release");

	for (const auto& acquireFence : myAcquireFences)
		acquireFence.Wait();
	myAcquireFences.clear();

	myFrames.clear();
	if (mySwapchain != nullptr)
	{
		InternalDestroySwapchain(mySwapchain);
		mySwapchain = nullptr;
	}
	myLastPresentId = 0;
	myHasPresented = false;
	myNeedsRecreate = true;
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
			InternalDestroySwapchain(mySwapchain);

		if (mySurface != nullptr)
		{
			Untrack(VK_OBJECT_TYPE_SURFACE_KHR, mySurface);
			vkDestroySurfaceKHR(
				GetInstance(),
				mySurface,
				&GetInstance().GetHostAllocationCallbacks());
		}
	}
}

template <>
Swapchain<kVk>& Swapchain<kVk>::operator=(Swapchain&& other) noexcept
{
	Swap(other);
	return *this;
}

} // namespace rhi
