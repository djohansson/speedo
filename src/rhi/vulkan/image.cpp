#include <rhi/device.h>
#include <rhi/image.h>
#include <rhi/rhiapplication.h>
#include <rhi/vulkan/utils.h>

#include <core/file.h>
#include <gfx/imageimport.h>

#include <filesystem>
#include <print>
#include <string_view>
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

//NOLINTBEGIN(readability-magic-numbers)
std::tuple<BufferHandle<kVk>, AllocationHandle<kVk>, ImageCreateDesc<kVk>> Load(
	std::string_view imageFile,
	Device<kVk>& device,
	std::atomic_uint8_t& progressOut,
	bool srgb)
{
	ZoneScopedN("image::load");

	std::tuple<BufferHandle<kVk>, AllocationHandle<kVk>, ImageCreateDesc<kVk>> initialData;

	auto& [bufferHandle, memoryHandle, desc] = initialData;

	desc.imageAspectFlags = VK_IMAGE_ASPECT_COLOR_BIT;

	// loading is given up when the application exits (only where it takes long; decoding itself can't be interrupted)
	auto app = core::Application::Get();
	ENSURE(app);
	auto cancelled = [&app] { return app->IsExitRequested(); };

	auto loadBin = [&imageFile, &initialData, &device, &progressOut](auto& inStream) -> std::error_code
	{
		progressOut = 32;

		auto& [bufferHandle, memoryHandle, desc] = initialData;

		if (auto result = inStream(desc); failure(result))
			return std::make_error_code(result);

		// the cached desc holds the uuid of the image it was saved from (see ObjectCreateDesc)
		desc.uuid = uuids::NewUuid();
		desc.name = std::string(imageFile);

		size_t size = 0;
		for (const auto& mipLevel : desc.mipLevels)
			size += mipLevel.size;

		auto [locBufferHandle, locMemoryHandle] = CreateBuffer(
			device.GetAllocator(),
			size,
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			nullptr);

		progressOut = 64;

		void* data;
		VK_CHECK(vmaMapMemory(device.GetAllocator(), locMemoryHandle, &data));
		auto result = inStream(std::span(static_cast<uint8_t*>(data), size));
		vmaUnmapMemory(device.GetAllocator(), locMemoryHandle);
		if (failure(result))
		{
			DestroyBuffer(device.GetAllocator(), locBufferHandle, locMemoryHandle);
			return std::make_error_code(result);
		}

		bufferHandle = locBufferHandle;
		memoryHandle = locMemoryHandle;

		progressOut = 255;

		return {};
	};

	auto saveBin = [&initialData, &device](auto& outStream) -> std::error_code
	{
		auto& [bufferHandle, memoryHandle, desc] = initialData;
		
		if (auto result = outStream(desc); failure(result))
			return std::make_error_code(result);

		size_t size = 0;
		for (const auto& mipLevel : desc.mipLevels)
			size += mipLevel.size;

		void* data;
		VK_CHECK(vmaMapMemory(device.GetAllocator(), memoryHandle, &data));
		auto result = outStream(std::span(static_cast<const uint8_t*>(data), size));
		vmaUnmapMemory(device.GetAllocator(), memoryHandle);
		if (failure(result))
			return std::make_error_code(result);

		// LoadAsset reports the rest, while hashing the saved cache
		return {};
	};

	auto loadImage = [&imageFile, &initialData, &device, &progressOut, &cancelled, srgb](auto& /*todo: use me: in*/) -> std::error_code
	{
		progressOut = 32;

		auto& [bufferHandle, memoryHandle, desc] = initialData;

		BufferHandle<kVk> locBufferHandle = nullptr;
		AllocationHandle<kVk> locMemoryHandle = nullptr;
		void* stagingBuffer = nullptr;
		auto allocate = [&](size_t size) -> std::byte*
		{
			std::tie(locBufferHandle, locMemoryHandle) = CreateBuffer(
				device.GetAllocator(),
				size,
				VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				nullptr);
			VK_CHECK(vmaMapMemory(device.GetAllocator(), locMemoryHandle, &stagingBuffer));
			return static_cast<std::byte*>(stagingBuffer);
		};

		auto image = gfx::image::Import(
			std::filesystem::path(imageFile),
			srgb ? gfx::image::ColorSpace::kSrgb : gfx::image::ColorSpace::kLinear,
			allocate,
			&progressOut,
			cancelled);

		if (stagingBuffer != nullptr)
			vmaUnmapMemory(device.GetAllocator(), locMemoryHandle);

		if (!image)
		{
			if (locBufferHandle != nullptr)
				DestroyBuffer(device.GetAllocator(), locBufferHandle, locMemoryHandle);

			if (cancelled())
				return std::make_error_code(std::errc::operation_canceled);

			std::println(stderr, "{}", image.error());
			return std::make_error_code(std::errc::invalid_argument);
		}

		desc.uuid = uuids::NewUuid();
		desc.name = std::string(imageFile);
		if (image->format == gfx::image::Format::kBC3)
			desc.format = srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
		else
			desc.format = srgb ? VK_FORMAT_BC1_RGB_SRGB_BLOCK : VK_FORMAT_BC1_RGB_UNORM_BLOCK;
		desc.usageFlags = VK_IMAGE_USAGE_SAMPLED_BIT;
		desc.mipLevels.resize(image->mipLevels.size());
		for (size_t levelIt = 0; levelIt < image->mipLevels.size(); levelIt++)
		{
			const auto& level = image->mipLevels[levelIt];
			desc.mipLevels[levelIt] = {
				.extent = {.width = level.width, .height = level.height}, .size = level.size, .offset = level.offset};
		}

		bufferHandle = locBufferHandle;
		memoryHandle = locMemoryHandle;

		return {};
	};

	std::string params;
	std::string paramsHash;
	params.append("stb_image-2.30|stb_image_resize-2.10|stb_dxt-1.12"); // todo: read version from stb headers
	params.append("|imageimport-v1"); // bump when gfx::image::Import changes what it produces
	params.append(srgb ? "|srgb" : "|linear");
	params.append("|cache-v2"); // bump when the serialized ImageCreateDesc layout changes, to invalidate stale caches
	static constexpr size_t kSha2Size = 32;
	std::array<uint8_t, kSha2Size> sha2;
	picosha2::hash256(params.cbegin(), params.cend(), sha2.begin(), sha2.end());
	picosha2::bytes_to_hex_string(sha2.cbegin(), sha2.cend(), paramsHash);
	auto loadResult = core::file::LoadAsset(imageFile, loadImage, loadBin, saveBin, paramsHash, {}, &progressOut, cancelled);

	if (!loadResult || bufferHandle == nullptr)
	{
		// cancelled or failed, possibly after creating the staging buffer (e.g. while hashing the cache)
		if (!loadResult && loadResult.error() != std::errc::operation_canceled)
			std::println(stderr, "Failed to load image {}: {}", imageFile, loadResult.error().message());

		if (bufferHandle != nullptr)
			DestroyBuffer(device.GetAllocator(), bufferHandle, memoryHandle);
		bufferHandle = nullptr;

		return initialData;
	}

	// runtime handles are not part of the serialized desc (and the loaders don't set them), so fill them in here
	desc.instance = device.GetDesc().instance;
	desc.device = device;

	return initialData;
}
//NOLINTEND(readability-magic-numbers)

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
	CommandBufferHandle<kVk> cmd,
	core::TaskCreateInfo<void>& timlineCallbackOut,
	std::tuple<BufferHandle<kVk>, AllocationHandle<kVk>, CreateDescType>&& initialDataAndDesc)
	: Image(
		std::forward<CreateDescType>(std::get<2>(initialDataAndDesc)),
		cmd,
		timlineCallbackOut,
		std::make_tuple(std::get<0>(initialDataAndDesc), std::get<1>(initialDataAndDesc)))
{}

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
	DeviceHandle<kVk> device,
	CommandBufferHandle<kVk> cmd,
	std::string_view imageFile,
	std::atomic_uint8_t& progressOut,
	core::TaskCreateInfo<void>& timlineCallbackOut)
	: Image(
		cmd,
		timlineCallbackOut,
		image::detail::Load(
			imageFile,
			GetDevice(device),
			progressOut,
			true))
{}

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

template <>
std::tuple<std::shared_ptr<Image<kVk>>, std::shared_ptr<ImageView<kVk>>>
Image<kVk>::LoadImage(DeviceHandle<kVk> deviceHandle, std::string_view filePath, std::atomic_uint8_t& progressOut, bool srgb)
{
	using namespace core;

	ZoneScopedN("Image::LoadImage");

	auto app = std::static_pointer_cast<RHIApplication>(Application::Get());
	ENSURE(app);
	auto& rhi = app->GetRHI<kVk>();
	auto& device = rhi.GetDevice(deviceHandle);

	// load into a staging buffer before taking the queue lock: on devices without a dedicated transfer queue it is the
	// graphics queue's lock, which Draw() takes every frame (see Device::GetQueue)
	auto initialDataAndDesc = image::detail::Load(filePath, device, progressOut, srgb);
	if (std::get<0>(initialDataAndDesc) == nullptr) // cancelled or failed
		return {};

	std::shared_ptr<Image<kVk>> image;
	std::shared_ptr<ImageView<kVk>> imageView;
	const Semaphore<kVk>* transferSemaphore = nullptr;
	uint64_t transferTimelineValue = 0;
	{
		auto transfer = device.GetQueue(kQueueTypeTransfer).Write();
		auto& [transferQueue, transferSubmits] = transfer->queues.Get();

		core::TaskCreateInfo<void> transferDone;
		// not make_shared: the constructor taking a staging buffer is private
		image = std::shared_ptr<Image<kVk>>(
			new Image<kVk>(transferQueue.GetPool().Commands(), transferDone, std::move(initialDataAndDesc)));
		imageView = std::make_shared<ImageView<kVk>>(
			ImageViewCreateDesc<kVk>{
				device.CreateDeviceObjectCreateDesc(filePath),
				*image,
				image->GetDesc().format,
				VK_IMAGE_ASPECT_COLOR_BIT});

		std::vector<core::TaskHandle> transferTimelineCallbacks;
		transferTimelineCallbacks.emplace_back(transferDone.handle);

		transferTimelineValue = ++transfer->timeline;
		transferQueue.EnqueueSubmit(QueueDeviceSyncInfo<kVk>{
			.waitSemaphores = {},
			.waitDstStageMasks = {},
			.waitSemaphoreValues = {},
			.signalSemaphores = {transfer->semaphore},
			.signalSemaphoreValues = {transferTimelineValue},
			.callbacks = std::move(transferTimelineCallbacks)});

		transferSubmits |= transferQueue.Submit();

		transferSemaphore = &transfer->semaphore;
	}

	// wait for the upload outside the queue lock. the caller is responsible for transitioning the image
	// to a shader readable layout (on a graphics queue) before using it.
	transferSemaphore->Wait(transferTimelineValue);

	return std::make_tuple(std::move(image), std::move(imageView));
}

} // namespace rhi
