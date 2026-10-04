#include <rhi/device.h>
#include <rhi/image.h>
#include <rhi/rhiapplication.h>
#include <rhi/vulkan/utils.h>

#include <core/file.h>
#include <core/math.h>

#include <execution>
#include <string_view>
#include <tuple>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_DXT_IMPLEMENTATION
#include <stb_dxt.h>

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>

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
	std::atomic_uint8_t& progressOut)
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
		auto result = inStream(std::span(static_cast<stbi_uc*>(data), size));
		vmaUnmapMemory(device.GetAllocator(), locMemoryHandle);
		if (failure(result))
			return std::make_error_code(result);

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
		auto result = outStream(std::span(static_cast<const stbi_uc*>(data), size));
		vmaUnmapMemory(device.GetAllocator(), memoryHandle);
		if (failure(result))
			return std::make_error_code(result);

		// LoadAsset reports the rest, while hashing the saved cache
		return {};
	};

	auto loadImage = [&imageFile, &initialData, &device, &progressOut, &cancelled](auto& /*todo: use me: in*/) -> std::error_code
	{
		progressOut = 32;

		auto& [bufferHandle, memoryHandle, desc] = initialData;

		int width;
		int height;
		int channelCount;
		stbi_uc* stbiImageData = stbi_load(imageFile.data(), &width, &height, &channelCount, STBI_rgb_alpha);

		uint32_t mipCount =
			static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;
		bool hasAlpha = channelCount == 4;
		uint32_t compressedBlockSize = hasAlpha ? 16 : 8;

		desc.uuid = uuids::NewUuid();
		desc.name = std::string(imageFile);
		desc.mipLevels.resize(mipCount);
		desc.format = channelCount == 4 ? VK_FORMAT_BC3_UNORM_BLOCK : VK_FORMAT_BC1_RGB_UNORM_BLOCK;
		desc.usageFlags = VK_IMAGE_USAGE_SAMPLED_BIT;

		uint32_t mipOffset = 0;
		for (uint32_t mipIt = 0; mipIt < mipCount; mipIt++)
		{
			uint32_t mipWidth = width >> mipIt;
			uint32_t mipHeight = height >> mipIt;
			auto mipSize = core::RoundUp(mipWidth, 4) * core::RoundUp(mipHeight, 4);

			if (!hasAlpha)
				mipSize >>= 1;

			desc.mipLevels[mipIt].extent = {.width = mipWidth, .height = mipHeight};
			desc.mipLevels[mipIt].size = mipSize;
			desc.mipLevels[mipIt].offset = mipOffset;

			mipOffset += mipSize;
		}

		auto [locBufferHandle, locMemoryHandle] = CreateBuffer(
			device.GetAllocator(),
			mipOffset,
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			nullptr);

		void* stagingBuffer;
		VK_CHECK(vmaMapMemory(device.GetAllocator(), locMemoryHandle, &stagingBuffer));

		auto compressBlocks = [&cancelled](const stbi_uc* src,
								 unsigned char* dst,
								 const Extent2d<kVk>& extent,
								 uint32_t compressedBlockSize,
								 bool hasAlpha,
								 uint32_t threadCount)
		{
			auto blockRowCount = extent.height >> 2;
			auto blockColCount = extent.width >> 2;
			auto blockCount = blockRowCount * blockColCount;

			auto extractBlock =
				[](const stbi_uc* src, size_t width, size_t stride, stbi_uc* dst)
			{
				for (size_t rowIt = 0; rowIt < 4; rowIt++)
				{
					std::copy(src, src + (stride * 4), &dst[rowIt * 16]);
					src += width * stride;
				}
			};

			std::atomic_uint32_t blockAtomic = 0;
			std::vector<uint32_t> threadIds(threadCount);
			std::ranges::iota(threadIds, 0);
			std::for_each_n(
				std::execution::par,
				threadIds.begin(),
				threadCount,
				[&](uint32_t /*threadId*/)
				{
					auto blockIt = blockAtomic++;
					while (blockIt < blockCount && !cancelled())
					{
						auto blockRowIt = blockIt / blockColCount;
						auto blockColIt = blockIt % blockColCount;
						auto rowIt = blockRowIt << 2;
						auto colIt = blockColIt << 2;
						auto srcOffset = ((rowIt * extent.width) + colIt) * 4;
						auto dstOffset = blockIt * compressedBlockSize;

						std::array<stbi_uc, 64> block;
						extractBlock(src + srcOffset, extent.width, 4, block.data());

						stb_compress_dxt_block(dst + dstOffset, block.data(), hasAlpha, STB_DXT_HIGHQUAL);

						blockIt = blockAtomic++;
					}
				});

			return blockCount * compressedBlockSize;
		};

		auto threadCount = std::thread::hardware_concurrency();
		auto* src = stbiImageData;
		auto* dst = static_cast<unsigned char*>(stagingBuffer);

		auto dprogress = 192 / (2 * desc.mipLevels.size());

		auto cancel = [&]
		{
			vmaUnmapMemory(device.GetAllocator(), locMemoryHandle);
			DestroyBuffer(device.GetAllocator(), locBufferHandle, locMemoryHandle);
			stbi_image_free(stbiImageData);
			return std::make_error_code(std::errc::operation_canceled);
		};

		dst += compressBlocks(
			src, dst, desc.mipLevels[0].extent, compressedBlockSize, hasAlpha, threadCount);

		progressOut += 2*dprogress;

		if (cancelled())
			return cancel();

		std::array<std::vector<stbi_uc>, 2> mipBuffers;
		for (size_t mipIt = 1; mipIt < desc.mipLevels.size(); mipIt++)
		{
			ZoneScopedN("image::loadImage::mip");

			auto previousMipIt = (mipIt - 1);
			auto currentBuffer = mipIt & 1;

			const auto& previousExtent = desc.mipLevels[previousMipIt].extent;
			const auto& currentExtent = desc.mipLevels[mipIt].extent;

			mipBuffers[currentBuffer].resize(
				std::max<size_t>(currentExtent.width, 4) *
				std::max<size_t>(currentExtent.height, 4) * 4);

			auto threadRowCount = previousExtent.height / threadCount;
			if (threadRowCount > 4)
			{
				std::vector<size_t> threadIds(threadCount);
				std::ranges::iota(threadIds, 0);
				std::for_each_n(
					std::execution::par,
					threadIds.begin(),
					threadCount,
					[&](size_t threadId)
					{
						ZoneScopedN("image::loadImage::mip::resize::thread");

						auto threadRowCountRest = (threadId == (threadCount - 1) ? previousExtent.height % threadCount : 0);

						stbir_resize_uint8_linear(
							src + (threadId * threadRowCount * previousExtent.width * 4),
							static_cast<int>(previousExtent.width),
							static_cast<int>(threadRowCount + threadRowCountRest),
							static_cast<int>(previousExtent.width * 4),
							mipBuffers[currentBuffer].data() +
								(threadId * (threadRowCount >> 1) * currentExtent.width * 4),
							static_cast<int>(currentExtent.width),
							static_cast<int>(((threadRowCount + threadRowCountRest) >> 1)),
							static_cast<int>(currentExtent.width * 4),
							STBIR_RGBA);
					});
			}
			else
			{
				ZoneScopedN("image::loadImage::mip::resize");

				stbir_resize_uint8_linear(
					src,
					static_cast<int>(previousExtent.width),
					static_cast<int>(previousExtent.height),
					static_cast<int>(previousExtent.width * 4),
					mipBuffers[currentBuffer].data(),
					static_cast<int>(currentExtent.width),
					static_cast<int>(currentExtent.height),
					static_cast<int>(currentExtent.width * 4),
					STBIR_RGBA);
			}

			progressOut += dprogress;

			src = mipBuffers[currentBuffer].data();
			dst +=
				compressBlocks(src, dst, currentExtent, compressedBlockSize, hasAlpha, threadCount);

			progressOut += dprogress;

			if (cancelled())
				return cancel();
		}

		vmaUnmapMemory(device.GetAllocator(), locMemoryHandle);
		stbi_image_free(stbiImageData);

		bufferHandle = locBufferHandle;
		memoryHandle = locMemoryHandle;

		return {};
	};

	std::string params;
	std::string paramsHash;
	params.append("stb_image-2.26|stb_image_resize-0.96|stb_dxt-1.10"); // todo: read version from stb headers
	params.append("|cache-v2"); // bump when the serialized ImageCreateDesc layout changes, to invalidate stale caches
	static constexpr size_t kSha2Size = 32;
	std::array<uint8_t, kSha2Size> sha2;
	picosha2::hash256(params.cbegin(), params.cend(), sha2.begin(), sha2.end());
	picosha2::bytes_to_hex_string(sha2.cbegin(), sha2.cend(), paramsHash);
	auto loadResult = core::file::LoadAsset(imageFile, loadImage, loadBin, saveBin, paramsHash, {}, &progressOut, cancelled);

	if (!loadResult && loadResult.error() == std::errc::operation_canceled)
	{
		// cancelled after the import created its staging buffer, i.e. while hashing the cache
		if (bufferHandle != nullptr)
			DestroyBuffer(device.GetAllocator(), bufferHandle, memoryHandle);
		bufferHandle = nullptr;

		return initialData;
	}

	ENSUREF(loadResult && bufferHandle != nullptr, "Failed to load image."); //NOLINT(readability-simplify-boolean-expr)

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
			progressOut))
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
Image<kVk>::LoadImage(DeviceHandle<kVk> deviceHandle, std::string_view filePath, std::atomic_uint8_t& progressOut)
{
	using namespace core;

	ZoneScopedN("Image::LoadImage");

	auto app = std::static_pointer_cast<RHIApplication>(Application::Get());
	ENSURE(app);
	auto& rhi = app->GetRHI<kVk>();
	auto& device = rhi.GetDevice(deviceHandle);

	// load into a staging buffer before taking the queue lock: on devices without a dedicated transfer queue it is the
	// graphics queue's lock, which Draw() takes every frame (see Device::GetQueue)
	auto initialDataAndDesc = image::detail::Load(filePath, device, progressOut);
	if (std::get<0>(initialDataAndDesc) == nullptr) // cancelled
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
