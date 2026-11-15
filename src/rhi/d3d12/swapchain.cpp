// the D3D12 backend's swapchains (docs/backends-plan.md, step 5): a DXGI flip model swapchain on the window (the
// platform's HWND, PlatformGetWin32Window) and images of the swapchain's own, a frame each, with the usage Vulkan's
// swapchain images have (a color attachment, a storage image, a copy's source and destination: DXGI's buffers take no
// unordered access), as Metal's backend has them. Flip takes the next in turn (nothing to acquire: one queue orders the
// frames' commands and their presents); a present copies the frame's image into DXGI's current buffer and presents it
// (Queue::Present). present modes: fifo (a sync interval of 1), mailbox (0: the flip model's newest frame at the next
// vertical blank) and immediate (0, tearing allowed, where the system allows it). the client waits for the device to be
// idle before it makes one again (a window's flip model swapchain is one at a time: the old one goes first)
#include <rhi/d3d12/backend.h>

#include <rhi/image.h>
#include <rhi/rhi.h>
#include <rhi/swapchain.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <algorithm>
#include <format>
#include <print>
#include <utility>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(Swapchain<kD3D12>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(Swapchain<kD3D12>);

namespace swapchain
{

// DXGI's flip model: 2 buffers at least, 16 at most
constexpr uint32_t kMinImageCount = 2;
constexpr uint32_t kMaxImageCount = 16;

// the window's client area, in pixels
static Extent2d WindowExtent(HWND window)
{
	RECT rect{};
	if (window == nullptr || GetClientRect(window, &rect) == FALSE)
		return {};
	return {.width = static_cast<uint32_t>(std::max(0L, rect.right - rect.left)), .height = static_cast<uint32_t>(std::max(0L, rect.bottom - rect.top))};
}

// whether the system lets a present tear (immediate: DXGI_FEATURE_PRESENT_ALLOW_TEARING, variable refresh displays)
static bool AllowsTearing(IDXGIFactory6* factory)
{
	BOOL allowed = FALSE;
	return SUCCEEDED(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowed, sizeof(allowed))) && allowed != FALSE;
}

} // namespace swapchain

template <>
const RenderTargetBeginInfo<kD3D12>& Swapchain<kD3D12>::Begin(CommandBufferHandle<kD3D12> cmd, SubpassContents contents)
{
	return myFrames[myFrameIndex].Begin(cmd, contents);
}

template <>
void Swapchain<kD3D12>::End(CommandBufferHandle<kD3D12> cmd)
{
	myFrames[myFrameIndex].End(cmd);
}

template <>
RenderTargetPassHandle<kD3D12> Swapchain<kD3D12>::GetHandle()
{
	return myFrames[myFrameIndex].GetHandle();
}

template <>
Extent2d Swapchain<kD3D12>::GetExtent() const
{
	return myFrames[myFrameIndex].GetExtent();
}

template <>
std::span<const ImageHandle<kD3D12>> Swapchain<kD3D12>::GetImages() const
{
	return myFrames[myFrameIndex].GetImages();
}

template <>
std::span<const ImageViewHandle<kD3D12>> Swapchain<kD3D12>::GetAttachments() const
{
	return myFrames[myFrameIndex].GetAttachments();
}

template <>
std::span<const AttachmentDescription<kD3D12>> Swapchain<kD3D12>::GetAttachmentDescs() const
{
	return myFrames[myFrameIndex].GetAttachmentDescs();
}

template <>
ImageLayout Swapchain<kD3D12>::GetLayout(uint32_t index) const
{
	return myFrames[myFrameIndex].GetLayout(index);
}

template <>
const std::optional<PipelineRenderingCreateInfo<kD3D12>>& Swapchain<kD3D12>::GetPipelineRenderingCreateInfo() const
{
	return myFrames[myFrameIndex].GetPipelineRenderingCreateInfo();
}

template <>
void Swapchain<kD3D12>::Blit(
	CommandBufferHandle<kD3D12> cmd,
	const IRenderTarget<kD3D12>& srcRenderTarget,
	const ImageSubresourceLayers<kD3D12>& srcSubresource,
	uint32_t srcIndex,
	const ImageSubresourceLayers<kD3D12>& dstSubresource,
	uint32_t dstIndex,
	Filter filter)
{
	myFrames[myFrameIndex].Blit(cmd, srcRenderTarget, srcSubresource, srcIndex, dstSubresource, dstIndex, filter);
}

template <>
void Swapchain<kD3D12>::Copy(
	CommandBufferHandle<kD3D12> cmd,
	const IRenderTarget<kD3D12>& srcRenderTarget,
	const ImageSubresourceLayers<kD3D12>& srcSubresource,
	uint32_t srcIndex,
	const ImageSubresourceLayers<kD3D12>& dstSubresource,
	uint32_t dstIndex)
{
	myFrames[myFrameIndex].Copy(cmd, srcRenderTarget, srcSubresource, srcIndex, dstSubresource, dstIndex);
}

template <>
void Swapchain<kD3D12>::ClearAll(CommandBufferHandle<kD3D12> cmd, std::span<const ClearValue> values) const
{
	myFrames[myFrameIndex].ClearAll(cmd, values);
}

template <>
void Swapchain<kD3D12>::ClearRect(CommandBufferHandle<kD3D12> cmd, const ClearValue& value, uint32_t index, const Rect& rect) const
{
	myFrames[myFrameIndex].ClearRect(cmd, value, index, rect);
}

template <>
void Swapchain<kD3D12>::Clear(CommandBufferHandle<kD3D12> cmd, const ClearValue& value, uint32_t index)
{
	myFrames[myFrameIndex].Clear(cmd, value, index);
}

template <>
void Swapchain<kD3D12>::Transition(CommandBufferHandle<kD3D12> cmd, ImageLayout layout, ImageAspect aspectFlags, uint32_t index)
{
	myFrames[myFrameIndex].Transition(cmd, layout, aspectFlags, index);
}

template <>
void Swapchain<kD3D12>::SetLoadOp(LoadOp loadOp, uint32_t index, LoadOp stencilLoadOp)
{
	myFrames[myFrameIndex].SetLoadOp(loadOp, index, stencilLoadOp);
}

template <>
void Swapchain<kD3D12>::SetStoreOp(StoreOp storeOp, uint32_t index, StoreOp stencilStoreOp)
{
	myFrames[myFrameIndex].SetStoreOp(storeOp, index, stencilStoreOp);
}

template <>
void Swapchain<kD3D12>::SetClearValue(const ClearValue& value, uint32_t index)
{
	myFrames[myFrameIndex].SetClearValue(value, index);
}


template <>
void Swapchain<kD3D12>::OnPresentResult(PresentResult result) noexcept
{
	if (result == PresentResult::kSuboptimal || result == PresentResult::kOutOfDate)
		myNeedsRecreate = true;
}

template <>
Extent2d Swapchain<kD3D12>::QuerySurfaceExtent()
{
	return swapchain::WindowExtent(mySurface);
}

template <>
FlipResult<kD3D12> Swapchain<kD3D12>::Flip()
{
	ZoneScopedN("Swapchain::Flip");

	// nothing to acquire (see the top of the file): the next frame in turn, its semaphore binary, which the queue skips.
	// a resized window asks for the swapchain to be made again (a flip model present reports none)
	auto lastFrameIndex = myFrameIndex;
	myFrameIndex = (myFrameIndex + 1) % static_cast<uint32_t>(myFrames.size());
	if (auto extent = QuerySurfaceExtent(); extent.width != GetDesc().extent.width || extent.height != GetDesc().extent.height)
		myNeedsRecreate = true;
	return FlipResult<kD3D12>{
		.acquireNextImageSemaphore = Semaphore<kD3D12>(
			SemaphoreCreateDesc<kD3D12>{SuperType::CreateDeviceObjectCreateDesc("acquireNextImageSemaphore"), SemaphoreType::kBinary}),
		.lastFrameIndex = lastFrameIndex,
		.newFrameIndex = myFrameIndex,
		.success = true};
}

template <>
QueuePresentInfo<kD3D12> Swapchain<kD3D12>::PreparePresent()
{
	ZoneScopedN("Swapchain::PreparePresent");

	auto presentInfo = myFrames[myFrameIndex].PreparePresent();
	presentInfo.swapchains.push_back(mySwapchain);
	myHasPresented = true;
	return presentInfo;
}

// the presents' ids are the swapchain's (Queue::Present signals each on its fence once its copy is done: the frame handed
// to DXGI, not yet shown, which DXGI doesn't report per present)
template <>
bool Swapchain<kD3D12>::WaitPresent(uint64_t presentId, uint64_t timeout) const
{
	auto* presents = mySwapchain->presents;
	if (presents->GetCompletedValue() >= presentId)
		return true;
	HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	ENSUREF(event != nullptr, "CreateEventW failed ({})", GetLastError());
	d3d12::Check(presents->SetEventOnCompletion(presentId, event), "ID3D12Fence::SetEventOnCompletion (a present)");
	constexpr uint64_t kNanosecondsPerMillisecond = 1'000'000;
	const DWORD milliseconds = timeout >= uint64_t{INFINITE} * kNanosecondsPerMillisecond
								   ? INFINITE
								   : static_cast<DWORD>((timeout + kNanosecondsPerMillisecond - 1) / kNanosecondsPerMillisecond);
	const bool presented = WaitForSingleObject(event, milliseconds) == WAIT_OBJECT_0;
	CloseHandle(event);
	return presented;
}

template <>
bool Swapchain<kD3D12>::WaitPresented(uint64_t timeout) const
{
	return mySwapchain == nullptr || mySwapchain->submitted == 0 || WaitPresent(mySwapchain->submitted, timeout);
}

template <>
std::pair<uint32_t, uint32_t> Swapchain<kD3D12>::GetImageCountLimits() const
{
	auto maxImageCount = GetDesc().maxImageCount != 0 ? std::min(swapchain::kMaxImageCount, GetDesc().maxImageCount) : swapchain::kMaxImageCount;
	return {swapchain::kMinImageCount, std::max(maxImageCount, swapchain::kMinImageCount)};
}

// (defined below, used here)
template <>
uint32_t Swapchain<kD3D12>::GetPresentationModes() const;

template <>
void Swapchain<kD3D12>::Configure(PresentationMode mode, uint32_t imageCount)
{
	if (((GetPresentationModes() >> static_cast<uint32_t>(mode)) & 1U) != 0)
	{
		auto& presentMode = InternalGetDesc().presentMode;
		presentMode = d3d12::PresentMode::kFifo;
		if (mode == PresentationMode::kMailbox)
			presentMode = d3d12::PresentMode::kMailbox;
		else if (mode == PresentationMode::kImmediate)
			presentMode = d3d12::PresentMode::kImmediate;
	}
	auto [minImageCount, maxImageCount] = GetImageCountLimits();
	InternalGetDesc().images.resize(std::clamp(imageCount, minImageCount, maxImageCount));
	myNeedsRecreate = true;
}

template <>
PresentationMode Swapchain<kD3D12>::GetPresentationMode() const
{
	switch (GetDesc().presentMode)
	{
	case d3d12::PresentMode::kMailbox: return PresentationMode::kMailbox;
	case d3d12::PresentMode::kImmediate: return PresentationMode::kImmediate;
	case d3d12::PresentMode::kFifo: break;
	}
	return PresentationMode::kFifo;
}

template <>
uint32_t Swapchain<kD3D12>::GetPresentationModes() const
{
	auto modes = (1U << static_cast<uint32_t>(PresentationMode::kFifo)) | (1U << static_cast<uint32_t>(PresentationMode::kMailbox));
	if (swapchain::AllowsTearing(static_cast<InstanceHandle<kD3D12>>(GetInstance())->factory))
		modes |= 1U << static_cast<uint32_t>(PresentationMode::kImmediate);
	return modes;
}

// exclusive fullscreen is DXGI's legacy one, which a flip model swapchain needn't (a borderless window covering its
// monitor is flipped independently, without the compositor): nothing here
template <>
void Swapchain<kD3D12>::SetExclusiveFullscreen(WindowHandle window) noexcept
{
	myExclusiveWindow = window;
}

template <>
void Swapchain<kD3D12>::InternalDestroySwapchain(SwapchainHandle<kD3D12> swapchain)
{
	if (swapchain == nullptr)
		return;
	// its presents' copies done (the client waits for the device to be idle before, which this doesn't rely on)
	if (swapchain->presents != nullptr && swapchain->presents->GetCompletedValue() < swapchain->submitted)
		d3d12::Check(
			swapchain->presents->SetEventOnCompletion(swapchain->submitted, nullptr), "ID3D12Fence::SetEventOnCompletion (a swapchain's presents)");
	for (auto& copy : swapchain->copies)
	{
		d3d12::Release(copy.list);
		d3d12::Release(copy.allocator);
	}
	for (auto*& buffer : swapchain->buffers)
		d3d12::Release(buffer);
	for (auto* image : swapchain->images)
		d3d12::DeleteImageObject(image);
	d3d12::Release(swapchain->presents);
	d3d12::Release(swapchain->swapchain);
	delete swapchain; //NOLINT(cppcoreguidelines-owning-memory)
}

template <>
bool Swapchain<kD3D12>::CreateSwapchain()
{
	ZoneScopedN("Swapchain::CreateSwapchain");

	auto extent = QuerySurfaceExtent();
	if (extent.width == 0 || extent.height == 0)
	{
		myNeedsRecreate = true;
		return false;
	}
	InternalGetDesc().extent = extent;

	// the old one first: a window has one flip model swapchain at a time
	myFrames.clear();
	InternalDestroySwapchain(mySwapchain);
	mySwapchain = nullptr;

	auto* device = static_cast<ID3D12Device10*>(GetDevice());
	auto* factory = static_cast<InstanceHandle<kD3D12>>(GetInstance())->factory;
	auto& allocator = d3d12::AllocatorObject::ForDevice(device);
	ENSUREF(allocator.directQueue != nullptr, "{}: a swapchain before the device's queues", GetName());
	const auto format = GetDesc().surfaceFormat.format;
	const auto imageCount = static_cast<uint32_t>(GetDesc().images.size());
	const bool tearing = swapchain::AllowsTearing(factory);

	auto* object = new d3d12::SwapchainObject{}; //NOLINT(cppcoreguidelines-owning-memory)
	switch (GetDesc().presentMode)
	{
	case d3d12::PresentMode::kFifo: object->syncInterval = 1; break;
	case d3d12::PresentMode::kMailbox: object->syncInterval = 0; break;
	case d3d12::PresentMode::kImmediate:
		object->syncInterval = 0;
		object->presentFlags = tearing ? DXGI_PRESENT_ALLOW_TEARING : 0U;
		break;
	}
	const DXGI_SWAP_CHAIN_DESC1 swapchainDesc{
		.Width = extent.width,
		.Height = extent.height,
		.Format = d3d12::ToD3D12(format),
		.Stereo = FALSE,
		.SampleDesc = {.Count = 1, .Quality = 0},
		.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
		.BufferCount = imageCount,
		.Scaling = DXGI_SCALING_STRETCH,
		.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
		.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED,
		.Flags = tearing ? static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) : 0U,
	};
	IDXGISwapChain1* swapchain1 = nullptr;
	d3d12::Check(
		factory->CreateSwapChainForHwnd(allocator.directQueue, mySurface, &swapchainDesc, nullptr, nullptr, &swapchain1),
		std::format("{}: CreateSwapChainForHwnd ({}x{}, {} buffers)", GetName(), extent.width, extent.height, imageCount));
	d3d12::Check(swapchain1->QueryInterface(IID_PPV_ARGS(&object->swapchain)), "IDXGISwapChain4");
	d3d12::Release(swapchain1);
	// the application switches fullscreen itself (the platform's borderless window), not DXGI on alt+enter
	(void)factory->MakeWindowAssociation(mySurface, DXGI_MWA_NO_ALT_ENTER);
	d3d12::Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&object->presents)), "CreateFence (a swapchain's presents)");
	d3d12::SetName(object->presents, std::format("{} Presents", GetName()));

	object->buffers.resize(imageCount);
	object->copies.resize(imageCount);
	object->images.reserve(imageCount);
	for (uint32_t imageIt = 0; imageIt < imageCount; imageIt++)
	{
		d3d12::Check(object->swapchain->GetBuffer(imageIt, IID_PPV_ARGS(&object->buffers[imageIt])), "IDXGISwapChain::GetBuffer");
		d3d12::SetName(object->buffers[imageIt], std::format("{} Buffer {}", GetName(), imageIt));
		auto& copy = object->copies[imageIt];
		d3d12::Check(
			device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&copy.allocator)), "CreateCommandAllocator (a present's copy)");
		d3d12::Check(
			device->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&copy.list)),
			"CreateCommandList1 (a present's copy)");
		d3d12::SetName(copy.list, std::format("{} Present {}", GetName(), imageIt));
		auto* image = d3d12::NewImageObject(ImageCreateDesc<kD3D12>{
			SuperType::CreateDeviceObjectCreateDesc(std::format("{} Image {}", GetName(), imageIt)),
			{extent.width, extent.height, 1},
			format,
			ImageTiling::kOptimal,
			ImageUsage::kColorAttachment | ImageUsage::kStorage | ImageUsage::kSampled | ImageUsage::kTransferSource |
				ImageUsage::kTransferDestination,
			MemoryProperty::kDeviceLocal,
			ImageAspect::kColor,
			ImageLayout::kUndefined});
		object->images.push_back(image);
		InternalGetDesc().images[imageIt] = image;
	}
	mySwapchain = object;

	myFrames.reserve(imageCount);
	for (uint32_t frameIt = 0; frameIt < imageCount; frameIt++)
		myFrames.emplace_back(FrameCreateDesc<kD3D12>{
			RenderTargetCreateDesc<kD3D12>{
				SuperType::CreateDeviceObjectCreateDesc(std::format("Frame{}", frameIt)),
				extent,
				{format},
				{ImageLayout::kUndefined},
				{ImageAspect::kColor},
				{object->images[frameIt]},
				{ClearValue{}},
				1,
				GetDesc().useDynamicRendering},
			frameIt});

	myFrameIndex = imageCount - 1;
	myNeedsRecreate = false;
	const auto mode = GetPresentationMode();
	std::println(
		"swapchain: {} images, {}x{}, {}",
		imageCount,
		extent.width,
		extent.height,
		mode == PresentationMode::kFifo ? "fifo" : (mode == PresentationMode::kMailbox ? "mailbox" : "immediate"));
	return true;
}

template <>
void Swapchain<kD3D12>::Release()
{
	ZoneScopedN("Swapchain::Release");

	myFrames.clear();
	InternalDestroySwapchain(mySwapchain);
	mySwapchain = nullptr;
	myNeedsRecreate = true;
}

template <>
void Swapchain<kD3D12>::Swap(Swapchain& rhs) noexcept
{
	DeviceObject<Swapchain<kD3D12>>::Swap(rhs);
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
Swapchain<kD3D12>::Swapchain(CreateDescType&& desc)
	: DeviceObject<Swapchain<kD3D12>>(std::forward<CreateDescType>(desc))
	, mySurface(GetDesc().surface)
{
	(void)CreateSwapchain();
}

template <>
Swapchain<kD3D12>::Swapchain(Swapchain&& other) noexcept
{
	Swap(other);
}

template <>
Swapchain<kD3D12>& Swapchain<kD3D12>::operator=(Swapchain&& other) noexcept
{
	Swap(other);
	return *this;
}

template <>
Swapchain<kD3D12>::~Swapchain()
{
	if (!IsValid())
		return;
	myFrames.clear();
	InternalDestroySwapchain(mySwapchain); // (the window is the platform's)
}

} // namespace rhi
