// the Metal backend's swapchains (docs/backends-plan.md, step 6): a window's CAMetalLayer (the platform's,
// PlatformCreateMetalLayer) and images of the swapchain's own, textures with the usage Vulkan's swapchain images have
// (a color attachment, a storage image, a blit's source and destination: a drawable's texture is no storage image), a
// frame each. Flip takes the next in turn (Metal has nothing to acquire: one queue orders the frames' commands and their
// presents); a present copies the frame's image into the layer's next drawable and presents it (Queue::Present), the
// drawables pacing the frames. present modes are the layer's display sync: fifo, or immediate without it (no mailbox).
// The layer's drawable size is the swapchain's extent, set when it is made: until it is made again after a resize, the
// layer scales its drawables to the window
#include <rhi/metal/backend.h>

#include <rhi/rhi.h>
#include <rhi/swapchain.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <CoreGraphics/CGGeometry.h>
#include <objc/message.h>
#include <objc/runtime.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <span>
#include <utility>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(Swapchain<kMetal>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(Swapchain<kMetal>);

namespace swapchain
{

// the drawables a layer keeps (CAMetalLayer's maximumDrawableCount: 2 or 3)
constexpr uint32_t kMinImageCount = 2;
constexpr uint32_t kMaxImageCount = 3;

// the layer's size in pixels: its bounds (the view's, which resizes it with the window) times its contents scale (the
// window's backing scale). metal-cpp has neither, so through the runtime as the platform's code does
Extent2d LayerExtent(CA::MetalLayer* layer)
{
	auto* object = reinterpret_cast<id>(layer);
	auto bounds = reinterpret_cast<CGRect (*)(id, SEL)>(objc_msgSend)(object, sel_registerName("bounds"));
	auto scale = reinterpret_cast<double (*)(id, SEL)>(objc_msgSend)(object, sel_registerName("contentsScale"));
	return {
		.width = static_cast<uint32_t>(std::lround(bounds.size.width * scale)),
		.height = static_cast<uint32_t>(std::lround(bounds.size.height * scale))};
}

} // namespace swapchain

template <>
const RenderTargetBeginInfo<kMetal>& Swapchain<kMetal>::Begin(CommandBufferHandle<kMetal> cmd, SubpassContents contents)
{
	return myFrames[myFrameIndex].Begin(cmd, contents);
}

template <>
void Swapchain<kMetal>::End(CommandBufferHandle<kMetal> cmd)
{
	myFrames[myFrameIndex].End(cmd);
}

template <>
RenderTargetPassHandle<kMetal> Swapchain<kMetal>::GetHandle()
{
	return myFrames[myFrameIndex].GetHandle();
}

template <>
Extent2d Swapchain<kMetal>::GetExtent() const
{
	return myFrames[myFrameIndex].GetExtent();
}

template <>
std::span<const ImageHandle<kMetal>> Swapchain<kMetal>::GetImages() const
{
	return myFrames[myFrameIndex].GetImages();
}

template <>
std::span<const ImageViewHandle<kMetal>> Swapchain<kMetal>::GetAttachments() const
{
	return myFrames[myFrameIndex].GetAttachments();
}

template <>
std::span<const AttachmentDescription<kMetal>> Swapchain<kMetal>::GetAttachmentDescs() const
{
	return myFrames[myFrameIndex].GetAttachmentDescs();
}

template <>
ImageLayout Swapchain<kMetal>::GetLayout(uint32_t index) const
{
	return myFrames[myFrameIndex].GetLayout(index);
}

template <>
const std::optional<PipelineRenderingCreateInfo<kMetal>>& Swapchain<kMetal>::GetPipelineRenderingCreateInfo() const
{
	return myFrames[myFrameIndex].GetPipelineRenderingCreateInfo();
}

template <>
void Swapchain<kMetal>::Blit(
	CommandBufferHandle<kMetal> cmd,
	const IRenderTarget<kMetal>& srcRenderTarget,
	const ImageSubresourceLayers<kMetal>& srcSubresource,
	uint32_t srcIndex,
	const ImageSubresourceLayers<kMetal>& dstSubresource,
	uint32_t dstIndex,
	Filter filter)
{
	myFrames[myFrameIndex].Blit(cmd, srcRenderTarget, srcSubresource, srcIndex, dstSubresource, dstIndex, filter);
}

template <>
void Swapchain<kMetal>::Copy(
	CommandBufferHandle<kMetal> cmd,
	const IRenderTarget<kMetal>& srcRenderTarget,
	const ImageSubresourceLayers<kMetal>& srcSubresource,
	uint32_t srcIndex,
	const ImageSubresourceLayers<kMetal>& dstSubresource,
	uint32_t dstIndex)
{
	myFrames[myFrameIndex].Copy(cmd, srcRenderTarget, srcSubresource, srcIndex, dstSubresource, dstIndex);
}

template <>
void Swapchain<kMetal>::ClearAll(CommandBufferHandle<kMetal> cmd, std::span<const ClearValue> values) const
{
	myFrames[myFrameIndex].ClearAll(cmd, values);
}

template <>
void Swapchain<kMetal>::ClearRect(CommandBufferHandle<kMetal> cmd, const ClearValue& value, uint32_t index, const Rect& rect) const
{
	myFrames[myFrameIndex].ClearRect(cmd, value, index, rect);
}

template <>
void Swapchain<kMetal>::Clear(CommandBufferHandle<kMetal> cmd, const ClearValue& value, uint32_t index)
{
	myFrames[myFrameIndex].Clear(cmd, value, index);
}

template <>
void Swapchain<kMetal>::Transition(CommandBufferHandle<kMetal> cmd, ImageLayout layout, ImageAspect aspectFlags, uint32_t index)
{
	myFrames[myFrameIndex].Transition(cmd, layout, aspectFlags, index);
}

template <>
void Swapchain<kMetal>::SetLoadOp(LoadOp loadOp, uint32_t index, LoadOp stencilLoadOp)
{
	myFrames[myFrameIndex].SetLoadOp(loadOp, index, stencilLoadOp);
}

template <>
void Swapchain<kMetal>::SetStoreOp(StoreOp storeOp, uint32_t index, StoreOp stencilStoreOp)
{
	myFrames[myFrameIndex].SetStoreOp(storeOp, index, stencilStoreOp);
}

template <>
void Swapchain<kMetal>::SetClearValue(const ClearValue& value, uint32_t index)
{
	myFrames[myFrameIndex].SetClearValue(value, index);
}

template <>
void Swapchain<kMetal>::OnPresentResult(PresentResult result) noexcept
{
	if (result == PresentResult::kSuboptimal || result == PresentResult::kOutOfDate)
		myNeedsRecreate = true;
}

template <>
Extent2d Swapchain<kMetal>::QuerySurfaceExtent()
{
	return swapchain::LayerExtent(mySurface);
}

template <>
FlipResult<kMetal> Swapchain<kMetal>::Flip()
{
	ZoneScopedN("Swapchain::Flip");

	// nothing to acquire (see the top of the file): the next frame in turn, its semaphore binary, which the queue skips.
	// a resized window asks for the swapchain to be made again (Metal reports no suboptimal or out of date present)
	auto lastFrameIndex = myFrameIndex;
	myFrameIndex = (myFrameIndex + 1) % static_cast<uint32_t>(myFrames.size());
	if (auto extent = QuerySurfaceExtent(); extent.width != GetDesc().extent.width || extent.height != GetDesc().extent.height)
		myNeedsRecreate = true;
	return FlipResult<kMetal>{
		.acquireNextImageSemaphore = Semaphore<kMetal>(
			SemaphoreCreateDesc<kMetal>{SuperType::CreateDeviceObjectCreateDesc("acquireNextImageSemaphore"), SemaphoreType::kBinary}),
		.lastFrameIndex = lastFrameIndex,
		.newFrameIndex = myFrameIndex,
		.success = true};
}

template <>
QueuePresentInfo<kMetal> Swapchain<kMetal>::PreparePresent()
{
	ZoneScopedN("Swapchain::PreparePresent");

	auto presentInfo = myFrames[myFrameIndex].PreparePresent();
	presentInfo.swapchains.push_back(mySwapchain);
	myHasPresented = true;
	return presentInfo;
}

// a present's showing isn't waited for (no present ids yet: a drawable's presented handler would tell): "can't tell"
template <>
bool Swapchain<kMetal>::WaitPresent(uint64_t /*presentId*/, uint64_t /*timeout*/) const
{
	return false;
}

template <>
bool Swapchain<kMetal>::WaitPresented(uint64_t /*timeout*/) const
{
	return false;
}

template <>
std::pair<uint32_t, uint32_t> Swapchain<kMetal>::GetImageCountLimits() const
{
	auto maxImageCount = GetDesc().maxImageCount != 0 ? std::min(swapchain::kMaxImageCount, GetDesc().maxImageCount) : swapchain::kMaxImageCount;
	return {swapchain::kMinImageCount, std::max(maxImageCount, swapchain::kMinImageCount)};
}

template <>
void Swapchain<kMetal>::Configure(PresentationMode mode, uint32_t imageCount)
{
	if (mode == PresentationMode::kFifo || mode == PresentationMode::kImmediate)
		InternalGetDesc().presentMode = mode == PresentationMode::kFifo ? metal::PresentMode::kFifo : metal::PresentMode::kImmediate;
	auto [minImageCount, maxImageCount] = GetImageCountLimits();
	InternalGetDesc().images.resize(std::clamp(imageCount, minImageCount, maxImageCount));
	myNeedsRecreate = true;
}

template <>
PresentationMode Swapchain<kMetal>::GetPresentationMode() const
{
	return GetDesc().presentMode == metal::PresentMode::kFifo ? PresentationMode::kFifo : PresentationMode::kImmediate;
}

template <>
uint32_t Swapchain<kMetal>::GetPresentationModes() const
{
	return (1U << static_cast<uint32_t>(PresentationMode::kFifo)) | (1U << static_cast<uint32_t>(PresentationMode::kImmediate));
}

// exclusive fullscreen is Windows' (VK_EXT_full_screen_exclusive): nothing here
template <>
void Swapchain<kMetal>::SetExclusiveFullscreen(WindowHandle window) noexcept
{
	myExclusiveWindow = window;
}

template <>
void Swapchain<kMetal>::InternalDestroySwapchain(SwapchainHandle<kMetal> swapchain)
{
	if (swapchain == nullptr)
		return;
	for (auto* texture : swapchain->textures)
		texture->release();
	delete swapchain;
}

template <>
bool Swapchain<kMetal>::CreateSwapchain()
{
	ZoneScopedN("Swapchain::CreateSwapchain");

	auto extent = QuerySurfaceExtent();
	if (extent.width == 0 || extent.height == 0)
	{
		myNeedsRecreate = true;
		return false;
	}
	InternalGetDesc().extent = extent;

	myFrames.clear();
	InternalDestroySwapchain(mySwapchain);
	mySwapchain = nullptr;

	auto* device = static_cast<MTL::Device*>(GetDevice());
	const auto format = GetDesc().surfaceFormat.format;
	const auto imageCount = static_cast<uint32_t>(GetDesc().images.size());
	auto* layer = mySurface;
	layer->setDevice(device);
	layer->setPixelFormat(metal::ToMetal(format));
	layer->setFramebufferOnly(false); // a blit's destination (Queue::Present)
	layer->setDrawableSize(CGSizeMake(extent.width, extent.height));
	layer->setMaximumDrawableCount(std::clamp(imageCount, swapchain::kMinImageCount, swapchain::kMaxImageCount));
	layer->setDisplaySyncEnabled(GetDesc().presentMode == metal::PresentMode::kFifo);

	auto* object = new metal::SwapchainObject{.layer = layer, .textures = {}};
	auto* descriptor = MTL::TextureDescriptor::texture2DDescriptor(metal::ToMetal(format), extent.width, extent.height, false);
	descriptor->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
	descriptor->setStorageMode(MTL::StorageModePrivate);
	for (uint32_t imageIt = 0; imageIt < imageCount; imageIt++)
	{
		MTL::Texture* texture = device->newTexture(descriptor);
		ENSUREF(texture != nullptr, "{}: Failed to create a swapchain image", GetName());
		texture->setLabel(NS::String::string(std::format("{} Image {}", GetName(), imageIt).c_str(), NS::UTF8StringEncoding));
		object->textures.push_back(texture);
		InternalGetDesc().images[imageIt] = texture;
	}
	mySwapchain = object;

	myFrames.reserve(imageCount);
	for (uint32_t frameIt = 0; frameIt < imageCount; frameIt++)
		myFrames.emplace_back(FrameCreateDesc<kMetal>{
			RenderTargetCreateDesc<kMetal>{
				SuperType::CreateDeviceObjectCreateDesc(std::format("Frame{}", frameIt)),
				extent,
				{format},
				{ImageLayout::kUndefined},
				{ImageAspect::kColor},
				{object->textures[frameIt]},
				{ClearValue{}},
				1,
				GetDesc().useDynamicRendering},
			frameIt});

	myFrameIndex = imageCount - 1;
	myNeedsRecreate = false;
	std::println("swapchain: {} images, {}x{}, {}", imageCount, extent.width, extent.height, GetPresentationMode() == PresentationMode::kFifo ? "fifo" : "immediate");
	return true;
}

template <>
void Swapchain<kMetal>::Release()
{
	ZoneScopedN("Swapchain::Release");

	myFrames.clear();
	InternalDestroySwapchain(mySwapchain);
	mySwapchain = nullptr;
	myNeedsRecreate = true;
}

template <>
void Swapchain<kMetal>::Swap(Swapchain& rhs) noexcept
{
	DeviceObject<Swapchain<kMetal>>::Swap(rhs);
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
	std::swap(myExclusiveRefused, rhs.myExclusiveRefused);
	std::swap(myExclusiveForeground, rhs.myExclusiveForeground);
}

template <>
Swapchain<kMetal>::Swapchain(CreateDescType&& desc)
	: DeviceObject<Swapchain<kMetal>>(std::forward<CreateDescType>(desc))
	, mySurface(GetDesc().surface)
{
	(void)CreateSwapchain();
}

template <>
Swapchain<kMetal>::Swapchain(Swapchain&& other) noexcept
{
	Swap(other);
}

template <>
Swapchain<kMetal>& Swapchain<kMetal>::operator=(Swapchain&& other) noexcept
{
	Swap(other);
	return *this;
}

template <>
Swapchain<kMetal>::~Swapchain()
{
	if (!IsValid())
		return;
	myFrames.clear();
	InternalDestroySwapchain(mySwapchain);
	if (mySurface != nullptr)
		mySurface->release(); // the platform's layer, retained for the swapchain (PlatformCreateMetalLayer)
}

} // namespace rhi
