#include "../image.h"

#include "utils.h"

#include <core/file.h>
#include <core/math.h>

#include <cstdint>
#if defined(__WINDOWS__)
#	include <execution>
#endif
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <tuple>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_DXT_IMPLEMENTATION
#include <stb_dxt.h>

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize.h>

#include <zpp_bits.h>

// NOLINTBEGIN(readability-identifier-naming.*, readability-magic-numbers)
auto serialize(const ImageCreateDesc<Vk>&) -> zpp::bits::members<6>;
// NOLINTEND(readability-identifier-naming.*, readability-magic-numbers)

namespace image
{

std::tuple<VkImage, VmaAllocation>
CreateImage2D(VmaAllocator allocator, const ImageCreateDesc<Vk>& desc)
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
		"todo_insert_proper_name",
		desc.initialLayout);
}

std::tuple<VkImage, VmaAllocation> CreateImage2D(
	VkCommandBuffer cmd, VmaAllocator allocator, VkBuffer buffer, const ImageCreateDesc<Vk>& desc)
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
		"todo_insert_proper_name",
		desc.initialLayout);
}

std::tuple<ImageCreateDesc<Vk>, BufferHandle<Vk>, AllocationHandle<Vk>> Load(
	const std::filesystem::path& imageFile,
	const std::shared_ptr<Device<Vk>>& device,
	std::atomic_uint8_t& progress)
{
	ZoneScopedN("image::load");

	std::tuple<ImageCreateDesc<Vk>, BufferHandle<Vk>, AllocationHandle<Vk>> descAndInitialData;

	auto& [desc, bufferHandle, memoryHandle] = descAndInitialData;

	auto loadBin = [&descAndInitialData, &device, &progress](auto& in) -> std::error_code
	{
		progress = 32;// NOLINT(readability-magic-numbers)

		auto& [desc, bufferHandle, memoryHandle] = descAndInitialData;

		if (auto result = in(desc); failure(result))
			return std::make_error_code(result);

		size_t size = 0;
		for (const auto& mipLevel : desc.mipLevels)
			size += mipLevel.size;

		auto [locBufferHandle, locMemoryHandle] = CreateBuffer(
			device->GetAllocator(),
			size,
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			"todo_insert_proper_name");

		progress = 64;// NOLINT(readability-magic-numbers)

		void* data;
		VK_CHECK(vmaMapMemory(device->GetAllocator(), locMemoryHandle, &data));
		auto result = in(std::span(static_cast<char*>(data), size));
		vmaUnmapMemory(device->GetAllocator(), locMemoryHandle);
		if (failure(result))
			return std::make_error_code(result);

		bufferHandle = locBufferHandle;
		memoryHandle = locMemoryHandle;

		progress = 255;// NOLINT(readability-magic-numbers)

		return {};
	};

	auto saveBin = [&descAndInitialData, &device, &progress](auto& out) -> std::error_code
	{
		auto& [desc, bufferHandle, memoryHandle] = descAndInitialData;
		
		if (auto result = out(desc); failure(result))
			return std::make_error_code(result);

		size_t size = 0;
		for (const auto& mipLevel : desc.mipLevels)
			size += mipLevel.size;

		void* data;
		VK_CHECK(vmaMapMemory(device->GetAllocator(), memoryHandle, &data));
		auto result = out(std::span(static_cast<const char*>(data), size));
		vmaUnmapMemory(device->GetAllocator(), memoryHandle);
		if (failure(result))
			return std::make_error_code(result);

		progress = 255;// NOLINT(readability-magic-numbers)

		return {};
	};

	auto loadImage = [&descAndInitialData, &device, &imageFile, &progress](auto& /*todo: use me: in*/) -> std::error_code
	{
		progress = 32;// NOLINT(readability-magic-numbers)

		auto& [desc, bufferHandle, memoryHandle] = descAndInitialData;

		int width;
		int height;
		int channelCount;
		stbi_uc* stbiImageData = stbi_load(imageFile.string().c_str(), &width, &height, &channelCount, STBI_rgb_alpha);

		uint32_t mipCount =
			static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;
		bool hasAlpha = channelCount == 4;
		uint32_t compressedBlockSize = hasAlpha ? 16 : 8;// NOLINT(readability-magic-numbers)

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
			"todo_insert_proper_name");

		void* stagingBuffer;
		VK_CHECK(vmaMapMemory(device->GetAllocator(), locMemoryHandle, &stagingBuffer));

		auto compressBlocks = [](const stbi_uc* src,
								 unsigned char* dst,
								 const Extent2d<Vk>& extent,
								 uint32_t compressedBlockSize,
								 bool hasAlpha,
								 uint32_t threadCount)
		{
			auto blockRowCount = extent.height >> 2;
			auto blockColCount = extent.width >> 2;
			auto blockCount = blockRowCount * blockColCount;

			auto extractBlock =
				[](const stbi_uc* src, uint32_t width, uint32_t stride, stbi_uc* dst)
			{
				for (uint32_t rowIt = 0; rowIt < 4; rowIt++)
				{
					std::copy(src, src + stride * 4, &dst[rowIt * 16]);
					src += width * stride;
				}
			};

			std::atomic_uint32_t blockAtomic = 0;
			std::vector<uint32_t> threadIds(threadCount);
			std::iota(threadIds.begin(), threadIds.end(), 0);
			std::for_each_n(
#if defined(__WINDOWS__)
				std::execution::par,
#endif
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

						stbi_uc block[64]{0};
						extractBlock(src + srcOffset, extent.width, 4, block);

						stb_compress_dxt_block(dst + dstOffset, block, hasAlpha, STB_DXT_HIGHQUAL);

						blockIt = blockAtomic++;
					}
				});

			return blockCount * compressedBlockSize;
		};

		auto threadCount = std::thread::hardware_concurrency();
		auto* src = stbiImageData;
		auto* dst = static_cast<unsigned char*>(stagingBuffer);

		auto dp = 192 / (2 * desc.mipLevels.size());

		dst += compressBlocks(
			src, dst, desc.mipLevels[0].extent, compressedBlockSize, hasAlpha, threadCount);

		progress += 2*dp;

		std::array<std::vector<stbi_uc>, 2> mipBuffers;
		for (uint32_t mipIt = 1; mipIt < desc.mipLevels.size(); mipIt++)
		{
			ZoneScopedN("image::loadImage::mip");

			uint32_t previousMipIt = (mipIt - 1);
			uint32_t currentBuffer = mipIt & 1;

			const auto& previousExtent = desc.mipLevels[previousMipIt].extent;
			const auto& currentExtent = desc.mipLevels[mipIt].extent;

			mipBuffers[currentBuffer].resize(
				std::max<uint32_t>(currentExtent.width, 4) *
				std::max<uint32_t>(currentExtent.height, 4) * 4);

			auto threadRowCount = previousExtent.height / threadCount;
			if (threadRowCount > 4)
			{
				std::vector<uint32_t> threadIds(threadCount);
				std::iota(threadIds.begin(), threadIds.end(), 0);
				std::for_each_n(
#if defined(__WINDOWS__)
					std::execution::par,
#endif
					threadIds.begin(),
					threadCount,
					[&](uint32_t threadId)
					{
						ZoneScopedN("image::loadImage::mip::resize::thread");

						uint32_t threadRowCountRest = (threadId == (threadCount - 1) ? previousExtent.height % threadCount : 0);

						stbir_resize_uint8(
							src + threadId * threadRowCount * previousExtent.width * 4,
							previousExtent.width,
							threadRowCount + threadRowCountRest,
							previousExtent.width * 4,
							mipBuffers[currentBuffer].data() +
								threadId * (threadRowCount >> 1) * currentExtent.width * 4,
							currentExtent.width,
							((threadRowCount + threadRowCountRest) >> 1),
							currentExtent.width * 4,
							4);
					});
			}
			else
			{
				ZoneScopedN("image::loadImage::mip::resize");

				stbir_resize_uint8(
					src,
					previousExtent.width,
					previousExtent.height,
					previousExtent.width * 4,
					mipBuffers[currentBuffer].data(),
					currentExtent.width,
					currentExtent.height,
					currentExtent.width * 4,
					4);
			}

			progress += dp;

			src = mipBuffers[currentBuffer].data();
			dst +=
				compressBlocks(src, dst, currentExtent, compressedBlockSize, hasAlpha, threadCount);

			progress += dp;
		}

		vmaUnmapMemory(device->GetAllocator(), locMemoryHandle);
		stbi_image_free(stbiImageData);

		bufferHandle = locBufferHandle;
		memoryHandle = locMemoryHandle;

		return {};
	};

	static constexpr char kLoaderType[] = "stb_image|stb_image_resize|stb_dxt";
	static constexpr char kLoaderVersion[] = "2.26|0.96|1.10";
	file::LoadAsset<kLoaderType, kLoaderVersion>(imageFile, loadImage, loadBin, saveBin);

	if (bufferHandle == nullptr)
		throw std::runtime_error("Failed to load image.");

	return descAndInitialData;
}

} // namespace image

template <>
void Image<Vk>::Transition(CommandBufferHandle<Vk> cmd, ImageLayout<Vk> layout)
{
	ZoneScopedN("Image::Transition");

	if (GetImageLayout() != layout)
	{
		TransitionImageLayout(
			cmd, *this, myDesc.format, GetImageLayout(), layout, myDesc.mipLevels.size());
		SetImageLayout(layout);
	}
}

template <>
void Image<Vk>::Clear(
	CommandBufferHandle<Vk> cmd,
	const ClearValue<Vk>& value,
	ClearType type,
	const std::optional<ImageSubresourceRange<Vk>>& range)
{
	ZoneScopedN("Image::clear");

	static const VkImageSubresourceRange kDefaultColorRange{
		VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
	static const VkImageSubresourceRange kDefaultDepthStencilRange{
		VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
		0,
		VK_REMAINING_MIP_LEVELS,
		0,
		VK_REMAINING_ARRAY_LAYERS};

	switch (type)
	{
	case ClearType::Color:
		vkCmdClearColorImage(
			cmd,
			static_cast<VkImage>(*this),
			GetImageLayout(),
			&value.color,
			1,
			range ? &range.value() : &kDefaultColorRange);
		break;
	case ClearType::DepthStencil:
		vkCmdClearDepthStencilImage(
			cmd,
			static_cast<VkImage>(*this),
			GetImageLayout(),
			&value.depthStencil,
			1,
			range ? &range.value() : &kDefaultDepthStencilRange);
		break;
	default:
		ASSERT(false);
		break;
	}
}

template <>
Image<Vk>::Image(Image&& other) noexcept
	: DeviceObject(std::forward<Image>(other))
	, myDesc(std::exchange(other.myDesc, {}))
	, myImage(std::exchange(other.myImage, {}))
{}

template <>
Image<Vk>::Image(
	const std::shared_ptr<Device<Vk>>& device, ImageCreateDesc<Vk>&& desc, ValueType&& data)
	: DeviceObject(
		  device,
		  {"_Image"},
		  1,
		  VK_OBJECT_TYPE_IMAGE,
		  reinterpret_cast<uint64_t*>(&std::get<0>(data)))
	, myDesc(std::forward<ImageCreateDesc<Vk>>(desc))
	, myImage(std::forward<ValueType>(data))
{}

template <>
Image<Vk>::Image(const std::shared_ptr<Device<Vk>>& device, ImageCreateDesc<Vk>&& desc)
	: Image(
		  device,
		  std::forward<ImageCreateDesc<Vk>>(desc),
		  std::tuple_cat(
			  image::CreateImage2D(device->GetAllocator(), desc),
			  std::make_tuple(desc.initialLayout)))
{}

template <>
Image<Vk>::Image(
	const std::shared_ptr<Device<Vk>>& device,
	Queue<Vk>& queue,
	uint64_t timelineValue,
	std::tuple<ImageCreateDesc<Vk>, BufferHandle<Vk>, AllocationHandle<Vk>>&& descAndInitialData)
	: Image(
		  device,
		  std::forward<ImageCreateDesc<Vk>>(std::get<0>(descAndInitialData)),
		  std::tuple_cat(
			  image::CreateImage2D(
				  queue.GetPool().Commands(),
				  device->GetAllocator(),
				  std::get<1>(descAndInitialData),
				  std::get<0>(descAndInitialData)),
			  std::make_tuple(std::get<0>(descAndInitialData).initialLayout)))
{
	queue.AddTimelineCallback(
	{
		timelineValue,
		[allocator = device->GetAllocator(), descAndInitialData](uint64_t)
		{
			vmaDestroyBuffer(
				allocator,
				std::get<1>(descAndInitialData),
				std::get<2>(descAndInitialData));
		}
	});
}

template <>
Image<Vk>::Image(
	const std::shared_ptr<Device<Vk>>& device,
	Queue<Vk>& queue,
	uint64_t timelineValue,
	ImageCreateDesc<Vk>&& desc,
	const void* initialData,
	size_t initialDataSize)
	: Image(
		  device,
		  queue,
		  timelineValue,
		  std::tuple_cat(
			  std::make_tuple(std::forward<ImageCreateDesc<Vk>>(desc)),
			  CreateStagingBuffer(
				  device->GetAllocator(), initialData, initialDataSize, "todo_insert_proper_name")))
{}

template <>
Image<Vk>::Image(
	const std::shared_ptr<Device<Vk>>& device,
	Queue<Vk>& queue,
	uint64_t timelineValue,
	const std::filesystem::path& imageFile,
	std::atomic_uint8_t& progress)
	: Image(device, queue, timelineValue, image::Load(imageFile, device, progress))
{}

template <>
Image<Vk>::~Image()
{
	if (ImageHandle<Vk> image = *this)
		vmaDestroyImage(GetDevice()->GetAllocator(), image, GetImageMemory());
}

template <>
ImageView<Vk>::ImageView(ImageView&& other) noexcept
	: DeviceObject(std::forward<ImageView>(other))
	, myView(std::exchange(other.myView, {}))
{}

template <>
ImageView<Vk>::ImageView(const std::shared_ptr<Device<Vk>>& device, ImageViewHandle<Vk>&& view)
	: DeviceObject(
		  device, {"_View"}, 1, VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<uint64_t*>(&view))
	, myView(std::forward<ImageViewHandle<Vk>>(view))
{}

template <>
ImageView<Vk>::ImageView(
	const std::shared_ptr<Device<Vk>>& device, const Image<Vk>& image, Flags<Vk> aspectFlags)
	: ImageView<Vk>(
		  device,
		  CreateImageView2D(
			  *device,
			  &device->GetInstance()->GetHostAllocationCallbacks(),
			  0, // "reserved for future use"
			  image,
			  image.GetDesc().format,
			  aspectFlags,
			  image.GetDesc().mipLevels.size()))
{}

template <>
ImageView<Vk>::~ImageView()
{
	if (ImageViewHandle<Vk> view = *this)
		vkDestroyImageView(*GetDevice(), view, &GetDevice()->GetInstance()->GetHostAllocationCallbacks());
}

template <>
ImageView<Vk>& ImageView<Vk>::operator=(ImageView&& other) noexcept
{
	DeviceObject::operator=(std::forward<ImageView>(other));
	myView = std::exchange(other.myView, {});
	return *this;
}

template <>
void ImageView<Vk>::Swap(ImageView& rhs) noexcept
{
	DeviceObject::Swap(rhs);
	std::swap(myView, rhs.myView);
}
