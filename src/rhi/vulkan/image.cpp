#include "../image.h"
#include "../rhiapplication.h"
#include "../shaders/capi.h"
#include "utils.h"

#include <core/file.h>
#include <core/math.h>
#include <core/std_extra.h>

#include <cstdint>
#include <execution>
#include <iostream>
#include <numeric>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_DXT_IMPLEMENTATION
#include <stb_dxt.h>

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>

#include <zpp_bits.h>

//NOLINTBEGIN(readability-identifier-naming)
[[nodiscard]] zpp::bits::members<std_extra::member_count<ImageCreateDesc<kVk>>()> serialize(const ImageCreateDesc<kVk>&);
//NOLINTEND(readability-identifier-naming)

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
		desc.name.data(),
		desc.initialLayout);
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
		desc.name.data(),
		desc.initialLayout);
}

//NOLINTBEGIN(readability-magic-numbers)
std::tuple<BufferHandle<kVk>, AllocationHandle<kVk>, ImageCreateDesc<kVk>> Load(
	const std::filesystem::path& imageFile,
	const std::shared_ptr<Device<kVk>>& device,
	std::atomic_uint8_t& progressOut)
{
	ZoneScopedN("image::load");

	std::tuple<BufferHandle<kVk>, AllocationHandle<kVk>, ImageCreateDesc<kVk>> initialData;

	auto& [bufferHandle, memoryHandle, desc] = initialData;

	desc.imageAspectFlags = VK_IMAGE_ASPECT_COLOR_BIT;

	auto loadBin = [&imageFile, &initialData, &device, &progressOut](auto& inStream) -> std::error_code
	{
		progressOut = 32;

		auto& [bufferHandle, memoryHandle, desc] = initialData;

		if (auto result = inStream(desc); failure(result))
			return std::make_error_code(result);

		thread_local static std::string name; // need to stay alive until the image object is fully constructed
		name = imageFile.filename().string().append(" loadBin staging");
		
		desc.name = name;

		size_t size = 0;
		for (const auto& mipLevel : desc.mipLevels)
			size += mipLevel.size;

		auto [locBufferHandle, locMemoryHandle] = CreateBuffer(
			device->GetAllocator(),
			size,
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			desc.name.data());

		progressOut = 64;

		void* data;
		VK_ENSURE(vmaMapMemory(device->GetAllocator(), locMemoryHandle, &data));
		auto result = inStream(std::span(static_cast<stbi_uc*>(data), size));
		vmaUnmapMemory(device->GetAllocator(), locMemoryHandle);
		if (failure(result))
			return std::make_error_code(result);

		bufferHandle = locBufferHandle;
		memoryHandle = locMemoryHandle;

		progressOut = 255;

		return {};
	};

	auto saveBin = [&initialData, &device, &progressOut](auto& outStream) -> std::error_code
	{
		auto& [bufferHandle, memoryHandle, desc] = initialData;
		
		if (auto result = outStream(desc); failure(result))
			return std::make_error_code(result);

		size_t size = 0;
		for (const auto& mipLevel : desc.mipLevels)
			size += mipLevel.size;

		void* data;
		VK_ENSURE(vmaMapMemory(device->GetAllocator(), memoryHandle, &data));
		auto result = outStream(std::span(static_cast<const stbi_uc*>(data), size));
		vmaUnmapMemory(device->GetAllocator(), memoryHandle);
		if (failure(result))
			return std::make_error_code(result);

		progressOut = 255;

		return {};
	};

	auto loadImage = [&imageFile, &initialData, &device, &progressOut](auto& /*todo: use me: in*/) -> std::error_code
	{
		progressOut = 32;

		auto& [bufferHandle, memoryHandle, desc] = initialData;

		int width;
		int height;
		int channelCount;
		stbi_uc* stbiImageData = stbi_load(imageFile.string().c_str(), &width, &height, &channelCount, STBI_rgb_alpha);

		uint32_t mipCount =
			static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;
		bool hasAlpha = channelCount == 4;
		uint32_t compressedBlockSize = hasAlpha ? 16 : 8;

		thread_local static std::string name; // need to stay alive until the image object is fully constructed
		name = imageFile.filename().string().append("loadImage staging");

		desc.name = name;
		desc.mipLevels.resize(mipCount);
		desc.format = channelCount == 4 ? VK_FORMAT_BC3_UNORM_BLOCK : VK_FORMAT_BC1_RGB_UNORM_BLOCK;
		desc.usageFlags = VK_IMAGE_USAGE_SAMPLED_BIT;

		uint32_t mipOffset = 0;
		for (uint32_t mipIt = 0; mipIt < mipCount; mipIt++)
		{
			uint32_t mipWidth = width >> mipIt;
			uint32_t mipHeight = height >> mipIt;
			auto mipSize = RoundUp(mipWidth, 4) * RoundUp(mipHeight, 4);

			if (!hasAlpha)
				mipSize >>= 1;

			desc.mipLevels[mipIt].extent = {mipWidth, mipHeight};
			desc.mipLevels[mipIt].size = mipSize;
			desc.mipLevels[mipIt].offset = mipOffset;

			mipOffset += mipSize;
		}

		auto [locBufferHandle, locMemoryHandle] = CreateBuffer(
			device->GetAllocator(),
			mipOffset,
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			desc.name.data());

		void* stagingBuffer;
		VK_ENSURE(vmaMapMemory(device->GetAllocator(), locMemoryHandle, &stagingBuffer));

		auto compressBlocks = [](const stbi_uc* src,
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
					std::copy(src, src + stride * 4, &dst[rowIt * 16]);
					src += width * stride;
				}
			};

			std::atomic_uint32_t blockAtomic = 0;
			std::vector<uint32_t> threadIds(threadCount);
			std::iota(threadIds.begin(), threadIds.end(), 0);
			std::for_each_n(
				std::execution::par,
				threadIds.begin(),
				threadCount,
				[&](uint32_t /*threadId*/)
				{
					auto blockIt = blockAtomic++;
					while (blockIt < blockCount)
					{
						auto blockRowIt = blockIt / blockColCount;
						auto blockColIt = blockIt % blockColCount;
						auto rowIt = blockRowIt << 2;
						auto colIt = blockColIt << 2;
						auto srcOffset = (rowIt * extent.width + colIt) * 4;
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

		dst += compressBlocks(
			src, dst, desc.mipLevels[0].extent, compressedBlockSize, hasAlpha, threadCount);

		progressOut += 2*dprogress;

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
				std::iota(threadIds.begin(), threadIds.end(), 0);
				std::for_each_n(
					std::execution::par,
					threadIds.begin(),
					threadCount,
					[&](size_t threadId)
					{
						ZoneScopedN("image::loadImage::mip::resize::thread");

						auto threadRowCountRest = (threadId == (threadCount - 1) ? previousExtent.height % threadCount : 0);

						stbir_resize_uint8_linear(
							src + threadId * threadRowCount * previousExtent.width * 4,
							static_cast<int>(previousExtent.width),
							static_cast<int>(threadRowCount + threadRowCountRest),
							static_cast<int>(previousExtent.width * 4),
							mipBuffers[currentBuffer].data() +
								threadId * (threadRowCount >> 1) * currentExtent.width * 4,
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
		}

		vmaUnmapMemory(device->GetAllocator(), locMemoryHandle);
		stbi_image_free(stbiImageData);

		bufferHandle = locBufferHandle;
		memoryHandle = locMemoryHandle;

		return {};
	};

	std::string params, paramsHash;
	params.append("stb_image-2.26|stb_image_resize-0.96|stb_dxt-1.10"); // todo: read version from stb headers
	static constexpr size_t kSha2Size = 32;
	std::array<uint8_t, kSha2Size> sha2;
	picosha2::hash256(params.cbegin(), params.cend(), sha2.begin(), sha2.end());
	picosha2::bytes_to_hex_string(sha2.cbegin(), sha2.cend(), paramsHash);
	auto loadResult = file::LoadAsset(imageFile, loadImage, loadBin, saveBin, paramsHash);

	ENSUREF(loadResult && bufferHandle != nullptr, "Failed to load image.");

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
		if (HasColorComponent(myDesc.format))
			aspectFlags |= VK_IMAGE_ASPECT_COLOR_BIT;
		else
		{
			if (HasDepthComponent(myDesc.format))
				aspectFlags |= VK_IMAGE_ASPECT_DEPTH_BIT;
			if (HasStencilComponent(myDesc.format))
				aspectFlags |= VK_IMAGE_ASPECT_STENCIL_BIT;
		}
	}

	if (GetLayout() != layout || GetAspectFlags() != aspectFlags)
	{
		TransitionImageLayout(
			cmd, *this, myDesc.format, GetLayout(), layout, myDesc.mipLevels.size(), aspectFlags);
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
		GetAspectFlags(), 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};

	if (GetAspectFlags() & VK_IMAGE_ASPECT_COLOR_BIT)
	{
		vkCmdClearColorImage(
			cmd,
			static_cast<VkImage>(*this),
			GetLayout(),
			&value.color,
			1,
			range ? &range.value() : &kDefaultRange);
	}
	else if (GetAspectFlags() & VK_IMAGE_ASPECT_DEPTH_BIT || GetAspectFlags() & VK_IMAGE_ASPECT_STENCIL_BIT)
	{
		vkCmdClearDepthStencilImage(
			cmd,
			static_cast<VkImage>(*this),
			GetLayout(),
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
Image<kVk>::Image(Image&& other) noexcept
	: DeviceObject(std::forward<Image>(other))
	, myDesc(std::exchange(other.myDesc, {}))
{
	std::swap(myImage, other.myImage);
}

template <>
Image<kVk>::Image(
	const std::shared_ptr<Device<kVk>>& device, ValueType&& data, ImageCreateDesc<kVk>&& desc)
	: DeviceObject(
		device,
		[&desc]{ return DeviceObjectCreateDesc{ desc.name.data() }; }(),
		1,
		VK_OBJECT_TYPE_IMAGE,
		reinterpret_cast<uint64_t*>(&std::get<0>(data)),
		uuids::uuid_system_generator{}())
	, myImage(std::forward<ValueType>(data))
	, myDesc(std::forward<ImageCreateDesc<kVk>>(desc))
{
	// Update the name to point to the DeviceObject's name.
	myDesc.name = GetName();
}

template <>
Image<kVk>::Image(const std::shared_ptr<Device<kVk>>& device, ImageCreateDesc<kVk>&& desc)
	: Image(
		device,
		std::tuple_cat(
			image::detail::CreateImage2D(device->GetAllocator(), desc),
			std::make_tuple(desc.initialLayout),
			std::make_tuple(desc.imageAspectFlags)),
		std::forward<ImageCreateDesc<kVk>>(desc))
{}

template <>
Image<kVk>::Image(
	const std::shared_ptr<Device<kVk>>& device,
	CommandBufferHandle<kVk> cmd,
	TaskCreateInfo<void>& timlineCallbackOut,
	std::tuple<BufferHandle<kVk>, AllocationHandle<kVk>, ImageCreateDesc<kVk>>&& initialData)
	: Image(
		device,
		std::tuple_cat(
			[&cmd, &device, &initialData]{
				return image::detail::CreateImage2D(cmd, device->GetAllocator(), std::get<0>(initialData), std::get<2>(initialData));
			}(),
			std::make_tuple(std::get<2>(initialData).initialLayout),
			std::make_tuple(std::get<2>(initialData).imageAspectFlags)),
		std::forward<ImageCreateDesc<kVk>>(std::get<2>(initialData)))
{
	timlineCallbackOut = CreateTask(
		[allocator = device->GetAllocator(), buffer = std::get<0>(initialData), memory = std::get<1>(initialData)]{
			vmaDestroyBuffer(allocator, buffer, memory); });
}

template <>
Image<kVk>::Image(
	const std::shared_ptr<Device<kVk>>& device,
	CommandBufferHandle<kVk> cmd,
	ImageCreateDesc<kVk>&& desc,
	const void* initialData,
	size_t initialDataSize,
	TaskCreateInfo<void>& timlineCallbackOut)
	: Image(
		device,
		cmd,
		timlineCallbackOut,
		std::tuple_cat(
			CreateStagingBuffer(
				device->GetAllocator(),
				initialData,
				initialDataSize,
				desc.name.data()),
			std::make_tuple(std::forward<ImageCreateDesc<kVk>>(desc))))
{}

template <>
Image<kVk>::Image(
	const std::shared_ptr<Device<kVk>>& device,
	CommandBufferHandle<kVk> cmd,
	const std::filesystem::path& imageFile,
	std::atomic_uint8_t& progressOut,
	TaskCreateInfo<void>& timlineCallbackOut)
	: Image(device, cmd, timlineCallbackOut, image::detail::Load(imageFile, device, progressOut))
{}

template <>
Image<kVk>::~Image()
{
	if (ImageHandle<kVk> image = *this)
		vmaDestroyImage(InternalGetDevice()->GetAllocator(), image, GetMemory());
}

template <>
Image<kVk>& Image<kVk>::operator=(Image<kVk>&& other) noexcept
{
	DeviceObject::operator=(std::forward<Image>(other));
	std::swap(myImage, other.myImage);
	myDesc = std::exchange(other.myDesc, {});
	return *this;
}

template <>
ImageView<kVk>::ImageView(ImageView&& other) noexcept
	: DeviceObject(std::forward<ImageView>(other))
{
	std::swap(myView, other.myView);
}

template <>
ImageView<kVk>::ImageView(const std::shared_ptr<Device<kVk>>& device, ImageViewHandle<kVk>&& view)
	: DeviceObject(
		  device,
		  {"_View"},
		  1,
		  VK_OBJECT_TYPE_IMAGE_VIEW,
		  reinterpret_cast<uint64_t*>(&view),
		  uuids::uuid_system_generator{}())
	, myView(std::forward<ImageViewHandle<kVk>>(view))
{}

template <>
ImageView<kVk>::ImageView(
	const std::shared_ptr<Device<kVk>>& device, const Image<kVk>& image, Flags<kVk> aspectFlags, Format<kVk> format)
	: ImageView<kVk>(
		  device,
		  CreateImageView2D(
			  *device,
			  &device->GetInstance()->GetHostAllocationCallbacks(),
			  0, // "reserved for future use"
			  image,
			  (format == VK_FORMAT_UNDEFINED ? image.GetDesc().format : format),
			  aspectFlags,
			  image.GetDesc().mipLevels.size()))
{}

template <>
ImageView<kVk>::~ImageView()
{
	if (auto* view = static_cast<ImageViewHandle<kVk>>(*this))
		vkDestroyImageView(*InternalGetDevice(), view, &InternalGetDevice()->GetInstance()->GetHostAllocationCallbacks());
}

template <>
ImageView<kVk>& ImageView<kVk>::operator=(ImageView&& other) noexcept
{
	DeviceObject::operator=(std::forward<ImageView>(other));
	std::swap(myView, other.myView);
	return *this;
}

template <>
void ImageView<kVk>::Swap(ImageView& rhs) noexcept
{
	DeviceObject::Swap(rhs);
	std::swap(myView, rhs.myView);
}

namespace image
{

template <>
std::pair<Image<kVk>, ImageView<kVk>> LoadImage(std::string_view filePath, std::atomic_uint8_t& progressOut)
{
	ZoneScopedN("image::LoadImage");

	auto app = std::static_pointer_cast<RHIApplication>(Application::Get().lock());
	ENSURE(app);
	auto& rhi = app->GetRHI<kVk>();
	auto& pipeline = rhi.pipeline;
	ENSURE(pipeline);

	auto transfer = ConcurrentWriteScope(rhi.queues[kQueueTypeTransfer]);
	auto& [transferQueue, transferSubmit] = transfer->queues.Get();

	TaskCreateInfo<void> transferDone;
	std::pair<Image<kVk>, ImageView<kVk>> result;
	auto& [image, imageView] = result;
	image = Image<kVk>(rhi.device, transferQueue.GetPool().Commands(), filePath, progressOut, transferDone);
	imageView = ImageView<kVk>(rhi.device, result.first, VK_IMAGE_ASPECT_COLOR_BIT);
	
	std::vector<TaskHandle> transferTimelineCallbacks;
	transferTimelineCallbacks.emplace_back(transferDone.handle);

	transferQueue.EnqueueSubmit(QueueDeviceSyncInfo<kVk>{
		.waitSemaphores = {},
		.waitDstStageMasks = {},
		.waitSemaphoreValues = {},
		.signalSemaphores = {transfer->semaphore},
		.signalSemaphoreValues = {++transfer->timeline},
		.callbacks = std::move(transferTimelineCallbacks)});

	transferSubmit |= transferQueue.Submit();

	///////////

	auto [transitionTask, transitionFuture] = CreateTask<QueueTimelineContextData<kVk>*>(
	[&rhi,
		image = std::make_unique<Image<kVk>>(std::move(image)),
		imageView = std::make_unique<ImageView<kVk>>(std::move(imageView)),
		&transferSemaphore = transfer->semaphore, &transferSubmit](QueueTimelineContextData<kVk>* graphics)
	{
		ZoneScopedN("image::LoadImage::transitionTask");

		auto& [graphicsQueue, graphicsSubmit] = graphics->queues.Get();

		auto cmd = graphicsQueue.GetPool().Commands();
		{
			GPU_SCOPE(cmd, graphicsQueue, Transition);

			image->Transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		}
		cmd.End();

		auto& pipeline = rhi.pipeline;
		ENSURE(pipeline);
		auto& resources = pipeline->GetResources();

		auto [setDescriptorTask, setDescriptorFuture] = CreateTask([&pipeline, imageView = static_cast<ImageViewHandle<kVk>>(*imageView), imageLayout = image->GetLayout()]()
		{
			pipeline->SetDescriptorData(
				"gTextures",
				DescriptorImageInfo<kVk>{{}, imageView,	imageLayout},
				DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES,
				15);
		});

		// a bit cryptic, but it's just a task that holds on to the old image&view in its capture group until task is destroyed
		auto [oldImageDestroyTask, oldImageDestroyFuture] = CreateTask(
		[
			oldImage = std::atomic_load(&resources.image),
			oldImageView = std::atomic_load(&resources.imageView)] {});

		std::vector<TaskHandle> transitionTimelineCallbacks;
		transitionTimelineCallbacks.emplace_back(setDescriptorTask);
		transitionTimelineCallbacks.emplace_back(oldImageDestroyTask);
		
		graphicsQueue.EnqueueSubmit(QueueDeviceSyncInfo<kVk>{
			.waitSemaphores = {transferSemaphore},
			.waitDstStageMasks = {VK_PIPELINE_STAGE_TRANSFER_BIT},
			.waitSemaphoreValues = {transferSubmit.maxTimelineValue},
			.signalSemaphores = {graphics->semaphore},
			.signalSemaphoreValues = {++graphics->timeline},
			.callbacks = std::move(transitionTimelineCallbacks)});

		graphicsSubmit |= graphicsQueue.Submit();

		std::atomic_store(
			&resources.image,
			std::make_shared<Image<kVk>>(std::move(*image))); // todo: move Image into rhi namespace
		std::atomic_store(
			&resources.imageView,
			std::make_shared<ImageView<kVk>>(std::move(*imageView)));
	});

	rhi.drawCalls.enqueue(transitionTask);

	///////////

	return result;
}

} // namespace image
