#include "../image.h"

#include "utils.h"

#include <core/file.h>
#include <core/math.h>
#include <core/std_extra.h>

#include <cstdint>
#include <execution>
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

//NOLINTBEGIN(readability-identifier-naming)
[[nodiscard]] zpp::bits::members<std_extra::member_count<ImageCreateDesc<kVk>>()> serialize(const ImageCreateDesc<kVk>&);
//NOLINTEND(readability-identifier-naming)

namespace image
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
		desc.name.data(),
		desc.initialLayout);
}

//NOLINTBEGIN(readability-magic-numbers)
std::tuple<ImageCreateDesc<kVk>, BufferHandle<kVk>, AllocationHandle<kVk>> Load(
	const std::filesystem::path& imageFile,
	const std::shared_ptr<Device<kVk>>& device,
	std::atomic_uint8_t& progress)
{
	ZoneScopedN("image::load");

	std::tuple<ImageCreateDesc<kVk>, BufferHandle<kVk>, AllocationHandle<kVk>> descAndInitialData;

	auto& [desc, bufferHandle, memoryHandle] = descAndInitialData;

	auto loadBin = [&imageFile, &descAndInitialData, &device, &progress](auto& inStream) -> std::error_code
	{
		progress = 32;

		auto& [desc, bufferHandle, memoryHandle] = descAndInitialData;

		if (auto result = inStream(desc); failure(result))
			return std::make_error_code(result);

		size_t size = 0;
		for (const auto& mipLevel : desc.mipLevels)
			size += mipLevel.size;

		auto [locBufferHandle, locMemoryHandle] = CreateBuffer(
			device->GetAllocator(),
			size,
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			imageFile.string().append("_staging").data());

		progress = 64;

		void* data;
		VK_CHECK(vmaMapMemory(device->GetAllocator(), locMemoryHandle, &data));
		auto result = inStream(std::span(static_cast<char*>(data), size));
		vmaUnmapMemory(device->GetAllocator(), locMemoryHandle);
		if (failure(result))
			return std::make_error_code(result);

		bufferHandle = locBufferHandle;
		memoryHandle = locMemoryHandle;

		progress = 255;

		return {};
	};

	auto saveBin = [&descAndInitialData, &device, &progress](auto& outStream) -> std::error_code
	{
		auto& [desc, bufferHandle, memoryHandle] = descAndInitialData;
		
		if (auto result = outStream(desc); failure(result))
			return std::make_error_code(result);

		size_t size = 0;
		for (const auto& mipLevel : desc.mipLevels)
			size += mipLevel.size;

		void* data;
		VK_CHECK(vmaMapMemory(device->GetAllocator(), memoryHandle, &data));
		auto result = outStream(std::span(static_cast<const char*>(data), size));
		vmaUnmapMemory(device->GetAllocator(), memoryHandle);
		if (failure(result))
			return std::make_error_code(result);

		progress = 255;

		return {};
	};

	auto loadImage = [&imageFile, &descAndInitialData, &device, &progress](auto& /*todo: use me: in*/) -> std::error_code
	{
		progress = 32;

		auto& [desc, bufferHandle, memoryHandle] = descAndInitialData;

		int width;
		int height;
		int channelCount;
		stbi_uc* stbiImageData = stbi_load(imageFile.string().c_str(), &width, &height, &channelCount, STBI_rgb_alpha);

		uint32_t mipCount =
			static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;
		bool hasAlpha = channelCount == 4;
		uint32_t compressedBlockSize = hasAlpha ? 16 : 8;

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
			imageFile.string().append("_staging").data());

		void* stagingBuffer;
		VK_CHECK(vmaMapMemory(device->GetAllocator(), locMemoryHandle, &stagingBuffer));

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

		progress += 2*dprogress;

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

						stbir_resize_uint8(
							src + threadId * threadRowCount * previousExtent.width * 4,
							static_cast<int>(previousExtent.width),
							static_cast<int>(threadRowCount + threadRowCountRest),
							static_cast<int>(previousExtent.width * 4),
							mipBuffers[currentBuffer].data() +
								threadId * (threadRowCount >> 1) * currentExtent.width * 4,
							static_cast<int>(currentExtent.width),
							static_cast<int>(((threadRowCount + threadRowCountRest) >> 1)),
							static_cast<int>(currentExtent.width * 4),
							4);
					});
			}
			else
			{
				ZoneScopedN("image::loadImage::mip::resize");

				stbir_resize_uint8(
					src,
					static_cast<int>(previousExtent.width),
					static_cast<int>(previousExtent.height),
					static_cast<int>(previousExtent.width * 4),
					mipBuffers[currentBuffer].data(),
					static_cast<int>(currentExtent.width),
					static_cast<int>(currentExtent.height),
					static_cast<int>(currentExtent.width * 4),
					4);
			}

			progress += dprogress;

			src = mipBuffers[currentBuffer].data();
			dst +=
				compressBlocks(src, dst, currentExtent, compressedBlockSize, hasAlpha, threadCount);

			progress += dprogress;
		}

		vmaUnmapMemory(device->GetAllocator(), locMemoryHandle);
		stbi_image_free(stbiImageData);

		bufferHandle = locBufferHandle;
		memoryHandle = locMemoryHandle;

		return {};
	};

	file::LoadAsset<
		std_extra::make_string_literal<"stb_image|stb_image_resize|stb_dxt">().data(),
		std_extra::make_string_literal<"2.26|0.96|1.10">().data()>(imageFile, loadImage, loadBin, saveBin);

	CHECKF(bufferHandle != nullptr, "Failed to load image.");

	return descAndInitialData;
}
//NOLINTEND(readability-magic-numbers)

} // namespace image

template <>
void Image<kVk>::Transition(CommandBufferHandle<kVk> cmd, ImageLayout<kVk> layout)
{
	ZoneScopedN("Image::Transition");

	if (GetImageLayout() != layout)
	{
		TransitionImageLayout(
			cmd, *this, myDesc.format, GetImageLayout(), layout, myDesc.mipLevels.size());
		InternalSetImageLayout(layout);
	}
}

template <>
void Image<kVk>::Clear(
	CommandBufferHandle<kVk> cmd,
	const ClearValue<kVk>& value,
	ClearType type,
	const std::optional<ImageSubresourceRange<kVk>>& range)
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
	case ClearType::kColor:
		vkCmdClearColorImage(
			cmd,
			static_cast<VkImage>(*this),
			GetImageLayout(),
			&value.color,
			1,
			range ? &range.value() : &kDefaultColorRange);
		break;
	case ClearType::kDepthStencil:
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
Image<kVk>::Image(Image&& other) noexcept
	: DeviceObject(std::forward<Image>(other))
	, myDesc(std::exchange(other.myDesc, {}))
	, myImage(std::exchange(other.myImage, {}))
{}

template <>
Image<kVk>::Image(
	const std::shared_ptr<Device<kVk>>& device, ImageCreateDesc<kVk>&& desc, ValueType&& data)
	: DeviceObject(
		  device,
		  {"_Image"},
		  1,
		  VK_OBJECT_TYPE_IMAGE,
		  reinterpret_cast<uint64_t*>(&std::get<0>(data)))
	, myDesc(std::forward<ImageCreateDesc<kVk>>(desc))
	, myImage(std::forward<ValueType>(data))
{}

template <>
Image<kVk>::Image(const std::shared_ptr<Device<kVk>>& device, ImageCreateDesc<kVk>&& desc)
	: Image(
		  device,
		  std::forward<ImageCreateDesc<kVk>>(desc),
		  std::tuple_cat(
			  image::CreateImage2D(device->GetAllocator(), desc),
			  std::make_tuple(desc.initialLayout)))
{}

template <>
Image<kVk>::Image(
	const std::shared_ptr<Device<kVk>>& device,
	Queue<kVk>& queue,
	uint64_t timelineValue,
	std::tuple<ImageCreateDesc<kVk>, BufferHandle<kVk>, AllocationHandle<kVk>>&& descAndInitialData)
	: Image(
		  device,
		  std::forward<ImageCreateDesc<kVk>>(std::get<0>(descAndInitialData)),
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
Image<kVk>::Image(
	const std::shared_ptr<Device<kVk>>& device,
	Queue<kVk>& queue,
	uint64_t timelineValue,
	ImageCreateDesc<kVk>&& desc,
	const void* initialData,
	size_t initialDataSize)
	: Image(
		  device,
		  queue,
		  timelineValue,
		  std::tuple_cat(
			  std::make_tuple(std::forward<ImageCreateDesc<kVk>>(desc)),
			  CreateStagingBuffer(
					device->GetAllocator(),
					initialData,
					initialDataSize,
					desc.name.data())))
{}

template <>
Image<kVk>::Image(
	const std::shared_ptr<Device<kVk>>& device,
	Queue<kVk>& queue,
	uint64_t timelineValue,
	const std::filesystem::path& imageFile,
	std::atomic_uint8_t& progress)
	: Image(device, queue, timelineValue, image::Load(imageFile, device, progress))
{}

template <>
Image<kVk>::~Image()
{
	if (ImageHandle<kVk> image = *this)
		vmaDestroyImage(InternalGetDevice()->GetAllocator(), image, GetImageMemory());
}

template <>
ImageView<kVk>::ImageView(ImageView&& other) noexcept
	: DeviceObject(std::forward<ImageView>(other))
	, myView(std::exchange(other.myView, {}))
{}

template <>
ImageView<kVk>::ImageView(const std::shared_ptr<Device<kVk>>& device, ImageViewHandle<kVk>&& view)
	: DeviceObject(
		  device, {"_View"}, 1, VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<uint64_t*>(&view))
	, myView(std::forward<ImageViewHandle<kVk>>(view))
{}

template <>
ImageView<kVk>::ImageView(
	const std::shared_ptr<Device<kVk>>& device, const Image<kVk>& image, Flags<kVk> aspectFlags)
	: ImageView<kVk>(
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
ImageView<kVk>::~ImageView()
{
	if (auto* view = static_cast<ImageViewHandle<kVk>>(*this))
		vkDestroyImageView(*InternalGetDevice(), view, &InternalGetDevice()->GetInstance()->GetHostAllocationCallbacks());
}

template <>
ImageView<kVk>& ImageView<kVk>::operator=(ImageView&& other) noexcept
{
	DeviceObject::operator=(std::forward<ImageView>(other));
	myView = std::exchange(other.myView, {});
	return *this;
}

template <>
void ImageView<kVk>::Swap(ImageView& rhs) noexcept
{
	DeviceObject::Swap(rhs);
	std::swap(myView, rhs.myView);
}
