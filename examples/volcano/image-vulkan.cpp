#include "image.h"
#include "file.h"
#include "vk-utils.h"

#include <cstdint>
#if defined(__WINDOWS__)
#include <execution>
#endif
#include <iostream>
#include <string>
#include <thread>
#include <tuple>

#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_DXT_IMPLEMENTATION
#include <stb_dxt.h>

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize.h>

namespace stbi_istream_callbacks
{
int read(void* user, char* data, int size)
{
	std::istream* stream = static_cast<std::istream*>(user);
	return stream->rdbuf()->sgetn(data, size);
}

void skip(void* user, int size)
{
	std::istream* stream = static_cast<std::istream*>(user);
	stream->seekg(size, std::ios::cur);
}

int eof(void* user)
{
	std::istream* stream = static_cast<std::istream*>(user);
	return stream->tellg() != std::istream::pos_type(-1);
}
} // namespace stbi_istream_callbacks

namespace image
{

std::tuple<
    ImageCreateDesc<Vk>,
    BufferHandle<Vk>,
    AllocationHandle<Vk>>
load(
    const std::filesystem::path& imageFile,
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext)
{
    ZoneScopedN("image::load");

    std::tuple<
        ImageCreateDesc<Vk>,
        BufferHandle<Vk>,
        AllocationHandle<Vk>> descAndInitialData = {};

    auto& [desc, bufferHandle, memoryHandle] = descAndInitialData;
    desc.name = imageFile.filename().generic_string();

    auto loadPBin = [&descAndInitialData, &deviceContext](std::istream& stream)
    {
        auto& [desc, bufferHandle, memoryHandle] = descAndInitialData;
        cereal::PortableBinaryInputArchive pbin(stream);
        pbin(desc);

        uint32_t size = 0;
        for (const auto& mipLevel : desc.mipLevels)
            size += mipLevel.size;

        std::string debugString;
        debugString.append(desc.name);
        debugString.append("_staging");
        
        auto [locBufferHandle, locMemoryHandle] = createBuffer(
            deviceContext->getAllocator(), size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            debugString.c_str());

        std::byte* data;
        VK_CHECK(vmaMapMemory(deviceContext->getAllocator(), locMemoryHandle, (void**)&data));
        pbin(cereal::binary_data(data, size));
        vmaUnmapMemory(deviceContext->getAllocator(), locMemoryHandle);

        bufferHandle = locBufferHandle;
        memoryHandle = locMemoryHandle;
    };

    auto savePBin = [&descAndInitialData, &deviceContext](std::ostream& stream)
    {
        auto& [desc, bufferHandle, memoryHandle] = descAndInitialData;
        cereal::PortableBinaryOutputArchive pbin(stream);
        pbin(desc);

        uint32_t size = 0;
        for (const auto& mipLevel : desc.mipLevels)
            size += mipLevel.size;

        std::byte* data;
        VK_CHECK(vmaMapMemory(deviceContext->getAllocator(), memoryHandle, (void**)&data));
        pbin(cereal::binary_data(data, size));
        vmaUnmapMemory(deviceContext->getAllocator(), memoryHandle);
    };

    auto loadImage = [&descAndInitialData, &deviceContext](std::istream& stream)
    {
        auto& [desc, bufferHandle, memoryHandle] = descAndInitialData;
        
        stbi_io_callbacks callbacks;
        callbacks.read = &stbi_istream_callbacks::read;
        callbacks.skip = &stbi_istream_callbacks::skip;
        callbacks.eof = &stbi_istream_callbacks::eof;
        
        int width, height, channelCount;
        stbi_uc* stbiImageData = stbi_load_from_callbacks(&callbacks, &stream, &width, &height, &channelCount, STBI_rgb_alpha);

        uint32_t mipLevelCount = static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;
        bool hasAlpha = channelCount == 4;
        uint32_t compressedBlockSize = hasAlpha ? 16 : 8;

        desc.mipLevels.resize(mipLevelCount);
        desc.format = channelCount == 4 ? VK_FORMAT_BC3_UNORM_BLOCK : VK_FORMAT_BC1_RGB_UNORM_BLOCK;
        desc.usage = VK_IMAGE_USAGE_SAMPLED_BIT;

        uint32_t mipOffset = 0;
        for (uint32_t mipIt = 0; mipIt < mipLevelCount; mipIt++)
        {
            uint32_t mipWidth = width >> mipIt;
            uint32_t mipHeight = height >> mipIt;
            auto mipSize = roundUp(mipWidth, 4) * roundUp(mipHeight, 4);
            
            if (!hasAlpha)
                mipSize >>= 1;
            
            desc.mipLevels[mipIt].extent = { mipWidth, mipHeight };
            desc.mipLevels[mipIt].size = mipSize;
            desc.mipLevels[mipIt].offset = mipOffset;

            mipOffset += mipSize;
        }
        
        std::string debugString;
        debugString.append(desc.name);
        debugString.append("_staging");
        
        auto [locBufferHandle, locMemoryHandle] = createBuffer(
            deviceContext->getAllocator(), mipOffset, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            debugString.c_str());

        std::byte* data;
        VK_CHECK(vmaMapMemory(deviceContext->getAllocator(), locMemoryHandle, (void**)&data));

        auto extractBlock = [](const stbi_uc* src, uint32_t width, uint32_t stride, stbi_uc* dst)
        {
            for (uint32_t rowIt = 0; rowIt < 4; rowIt++)
            {
                std::copy(src, src + stride * 4, &dst[rowIt * 16]);
                src += width * stride;
            }
        };

        stbi_uc block[64] = {0};
        const stbi_uc* src = stbiImageData;
        stbi_uc* dst = reinterpret_cast<stbi_uc*>(data);

    //     auto threadCount = std::thread::hardware_concurrency();
    //     std::array<uint32_t, 128> seq;
    //     std::iota(seq.begin(), seq.begin() + threadCount, 0);
    //     std::for_each_n(
    // #if defined(__WINDOWS__)
    //         std::execution::par,
    // #endif
    //         seq.begin(), threadCount,
    //         [](uint32_t threadIt)
    //         {
    //             (void)threadIt;
    //         });
        
        const auto& mip0Extent = desc.mipLevels[0].extent;
        for (uint32_t rowIt = 0; rowIt < mip0Extent.height; rowIt += 4)
        {
            for (uint32_t colIt = 0; colIt < mip0Extent.width; colIt += 4)
            {
                uint32_t offset = (rowIt * mip0Extent.width + colIt) * 4;
                extractBlock(src + offset, mip0Extent.width, 4, block);
                stb_compress_dxt_block(dst, block, hasAlpha, STB_DXT_HIGHQUAL);
                dst += compressedBlockSize;
            }
        }

        std::array<std::vector<stbi_uc>, 2> mipBuffers;
        for (uint32_t mipIt = 1; mipIt < desc.mipLevels.size(); mipIt++)
        {
            uint32_t previousMipIt = (mipIt-1);
            uint32_t currentBuffer = ~previousMipIt & 1;

            const auto& previousExtent = desc.mipLevels[previousMipIt].extent;
            const auto& currentExtent = desc.mipLevels[mipIt].extent;
            
            mipBuffers[currentBuffer].resize(
                std::max<uint32_t>(currentExtent.width, 4) * std::max<uint32_t>(currentExtent.height, 4) * 4);
            
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

            src = mipBuffers[currentBuffer].data();

            for (uint32_t rowIt = 0; rowIt < currentExtent.height; rowIt += 4)
            {
                for (uint32_t colIt = 0; colIt < currentExtent.width; colIt += 4)
                {
                    uint32_t offset = (rowIt * currentExtent.width + colIt) * 4;
                    extractBlock(src + offset, currentExtent.width, 4, block);
                    stb_compress_dxt_block(dst, block, hasAlpha, STB_DXT_HIGHQUAL);
                    dst += compressedBlockSize;
                }
            }
        }

        vmaUnmapMemory(deviceContext->getAllocator(), locMemoryHandle);
        stbi_image_free(stbiImageData);

        bufferHandle = locBufferHandle;
        memoryHandle = locMemoryHandle;
    };

    loadCachedSourceFile(
        imageFile, imageFile, "stb_image|stb_image_resize|stb_dxt", "2.26|0.96|1.10", loadImage, loadPBin, savePBin);

    if (!bufferHandle)
        throw std::runtime_error("Failed to load image.");

    return descAndInitialData;
}

}

template <>
void Image<Vk>::transition(
    CommandBufferHandle<Vk> cmd,
    ImageLayout<Vk> layout)
{
    ZoneScopedN("Image::transition");

    if (getImageLayout() != layout)
    {
        transitionImageLayout(cmd, getImageHandle(), myDesc.format, getImageLayout(), layout, myDesc.mipLevels.size());
        std::get<2>(myData) = layout;
    }
}

template <>
Image<Vk>::Image(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    std::tuple<
        ImageCreateDesc<Vk>,
        ImageHandle<Vk>,
        AllocationHandle<Vk>,
        ImageLayout<Vk>>&& descAndData)
: DeviceResource<Vk>(
    deviceContext,
    std::get<0>(descAndData),
    1,
    VK_OBJECT_TYPE_IMAGE,
    reinterpret_cast<uint64_t*>(&std::get<1>(descAndData)))
, myDesc(std::move(std::get<0>(descAndData)))
, myData(std::make_tuple(std::get<1>(descAndData), std::get<2>(descAndData), std::get<3>(descAndData)))
{
}

template <>
Image<Vk>::Image(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    ImageCreateDesc<Vk>&& desc)
: Image(
    deviceContext,
    std::tuple_cat(
        std::make_tuple(std::move(desc)),
        createImage2D(
            deviceContext->getAllocator(),
            desc.mipLevels[0].extent.width,
            desc.mipLevels[0].extent.height,
            desc.mipLevels.size(),
            desc.format, 
            VK_IMAGE_TILING_OPTIMAL,
            desc.usage,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            desc.name.c_str()),
        std::make_tuple(VK_IMAGE_LAYOUT_UNDEFINED)))
{
}

template <>
Image<Vk>::Image(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    const std::shared_ptr<CommandContext<Vk>>& commandContext,
    std::tuple<
        ImageCreateDesc<Vk>,
        BufferHandle<Vk>,
        AllocationHandle<Vk>>&& descAndInitialData)
: Image(
    deviceContext,
    std::tuple_cat(
        std::make_tuple(std::move(std::get<0>(descAndInitialData))),
        createImage2D(
            commandContext->commands(),
            deviceContext->getAllocator(),
            std::get<1>(descAndInitialData),
            std::get<0>(descAndInitialData).mipLevels[0].extent.width,
            std::get<0>(descAndInitialData).mipLevels[0].extent.height,
            std::get<0>(descAndInitialData).mipLevels.size(),
            &std::get<0>(descAndInitialData).mipLevels[0].offset,
            sizeof(ImageMipLevelDesc<Vk>) / sizeof(uint32_t),
            std::get<0>(descAndInitialData).format, 
            VK_IMAGE_TILING_OPTIMAL,
            std::get<0>(descAndInitialData).usage,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            std::get<0>(descAndInitialData).name.c_str()),
        std::make_tuple(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)))
{   
    commandContext->addSubmitFinishedCallback([deviceContext, descAndInitialData](uint64_t){
        vmaDestroyBuffer(deviceContext->getAllocator(), std::get<1>(descAndInitialData), std::get<2>(descAndInitialData));
    });
}

template <>
Image<Vk>::Image(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    const std::shared_ptr<CommandContext<Vk>>& commandContext,
    const std::filesystem::path& imageFile)
: Image(deviceContext, commandContext, image::load(imageFile, deviceContext))
{
}

template <>
Image<Vk>::~Image()
{
    if (auto image = getImageHandle(); image)
        getDeviceContext()->addTimelineCallback(
            [allocator = getDeviceContext()->getAllocator(), image, imageMemory = getImageMemory()](uint64_t){
                vmaDestroyImage(allocator, image, imageMemory);
        });
}

template <>
ImageView<Vk>::ImageView(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    ImageViewHandle<Vk>&& imageView)
: DeviceResource<Vk>(
    deviceContext,
    {"_View"},
    1,
    VK_OBJECT_TYPE_IMAGE_VIEW,
    reinterpret_cast<uint64_t*>(&imageView))
, myImageView(std::move(imageView))
{
}

template <>
ImageView<Vk>::ImageView(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    const Image<Vk>& image,
    Flags<Vk> aspectFlags)
: ImageView<Vk>(
    deviceContext,
    createImageView2D(
        deviceContext->getDevice(),
        0, // "reserved for future use"
        image.getImageHandle(),
        image.getDesc().format,
        aspectFlags,
        image.getDesc().mipLevels.size()))
{
}

template <>
ImageView<Vk>::~ImageView()
{
    if (auto imageView = getImageViewHandle(); imageView)
        getDeviceContext()->addTimelineCallback(
            [device = getDeviceContext()->getDevice(), imageView](uint64_t){
                vkDestroyImageView(device, imageView, nullptr);
        });
}
