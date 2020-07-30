#include "image.h"
#include "file.h"
#include "vk-utils.h"

#include <core/slang-secure-crt.h>

#include <cstdint>
#include <iostream>
#include <string>
#include <tuple>

#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_DXT_IMPLEMENTATION
#include <stb_dxt.h>

#include <cereal/archives/binary.hpp>
#include <cereal/archives/portable_binary.hpp>
#include <cereal/cereal.hpp>
#include <cereal/types/map.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/utility.hpp>
#include <cereal/types/vector.hpp>

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
    ImageCreateDesc<GraphicsBackend::Vulkan>,
    BufferHandle<GraphicsBackend::Vulkan>,
    AllocationHandle<GraphicsBackend::Vulkan>>
load(
    const std::filesystem::path& imageFile,
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext)
{
    ZoneScopedN("image::load()");

    DeviceSize<GraphicsBackend::Vulkan> size = 0;

    std::tuple<
        ImageCreateDesc<GraphicsBackend::Vulkan>,
        BufferHandle<GraphicsBackend::Vulkan>,
        AllocationHandle<GraphicsBackend::Vulkan>> descAndInitialData = {};

    auto& [desc, bufferHandle, memoryHandle] = descAndInitialData;
    desc.name = imageFile.filename().generic_string();

    int channelCount = 0;

    auto loadPBin = [&descAndInitialData, &channelCount, &size, &deviceContext](std::istream& stream) {
        auto& [desc, bufferHandle, memoryHandle] = descAndInitialData;
        cereal::PortableBinaryInputArchive pbin(stream);
        pbin(desc.extent.width, desc.extent.height, channelCount, size);

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

    auto savePBin = [&descAndInitialData, &channelCount, &size, &deviceContext](std::ostream& stream) {
        auto& [desc, bufferHandle, memoryHandle] = descAndInitialData;
        cereal::PortableBinaryOutputArchive pbin(stream);
        pbin(desc.extent.width, desc.extent.height, channelCount, size);

        std::byte* data;
        VK_CHECK(vmaMapMemory(deviceContext->getAllocator(), memoryHandle, (void**)&data));
        pbin(cereal::binary_data(data, size));
        vmaUnmapMemory(deviceContext->getAllocator(), memoryHandle);
    };

    auto loadImage = [&descAndInitialData, &channelCount, &size, &deviceContext](std::istream& stream) {
        auto& [desc, bufferHandle, memoryHandle] = descAndInitialData;
        stbi_io_callbacks callbacks;
        callbacks.read = &stbi_istream_callbacks::read;
        callbacks.skip = &stbi_istream_callbacks::skip;
        callbacks.eof = &stbi_istream_callbacks::eof;
        stbi_uc* stbiImageData =
            stbi_load_from_callbacks(&callbacks, &stream, (int*)&desc.extent.width, (int*)&desc.extent.height, &channelCount, STBI_rgb_alpha);

        bool hasAlpha = channelCount == 4;
        uint32_t compressedBlockSize = hasAlpha ? 16 : 8;
        size = hasAlpha ? 2 * desc.extent.width * desc.extent.height : desc.extent.width * desc.extent.height / 2;

        std::string debugString;
        debugString.append(desc.name);
        debugString.append("_staging");
        
        auto [locBufferHandle, locMemoryHandle] = createBuffer(
            deviceContext->getAllocator(), size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            debugString.c_str());

        std::byte* data;
        VK_CHECK(vmaMapMemory(deviceContext->getAllocator(), locMemoryHandle, (void**)&data));

        auto extractBlock = [](const stbi_uc* src, uint32_t width, uint32_t stride,
                                stbi_uc* dst) {
            for (uint32_t rowIt = 0; rowIt < 4; rowIt++)
            {
                std::copy(src, src + stride * 4, &dst[rowIt * 16]);
                src += width * stride;
            }
        };

        stbi_uc block[64] = {0};
        const stbi_uc* src = stbiImageData;
        stbi_uc* dst = reinterpret_cast<stbi_uc*>(data);
        for (uint32_t rowIt = 0; rowIt < desc.extent.height; rowIt += 4)
        {
            for (uint32_t colIt = 0; colIt < desc.extent.width; colIt += 4)
            {
                uint32_t offset = (rowIt * desc.extent.width + colIt) * 4;
                extractBlock(src + offset, desc.extent.width, 4, block);
                stb_compress_dxt_block(dst, block, hasAlpha, STB_DXT_HIGHQUAL);
                dst += compressedBlockSize;
            }
        }

        vmaUnmapMemory(deviceContext->getAllocator(), locMemoryHandle);
        stbi_image_free(stbiImageData);

        bufferHandle = locBufferHandle;
        memoryHandle = locMemoryHandle;
    };

    loadCachedSourceFile(
        imageFile, imageFile, "stb_image|stb_dxt", "2.20|1.08b", loadImage, loadPBin, savePBin);

     // todo: write utility functions...
    desc.format = channelCount == 4 ? VK_FORMAT_BC2_UNORM_BLOCK : VK_FORMAT_BC1_RGB_UNORM_BLOCK;
    desc.usage = VK_IMAGE_USAGE_SAMPLED_BIT;

    uint32_t pixelSizeBytesDivisor;
    if (!bufferHandle || size != desc.extent.width * desc.extent.height * getFormatSize(desc.format, pixelSizeBytesDivisor) / pixelSizeBytesDivisor)
        throw std::runtime_error("Failed to load image.");

    return descAndInitialData;
}

}

template <>
void Image<GraphicsBackend::Vulkan>::transition(
    CommandBufferHandle<GraphicsBackend::Vulkan> cmd,
    ImageLayout<GraphicsBackend::Vulkan> layout)
{
    if (getImageLayout() != layout)
    {
        transitionImageLayout(cmd, getImageHandle(), myDesc.format, getImageLayout(), layout);
        std::get<2>(myData) = layout;
    }
}

template <>
void Image<GraphicsBackend::Vulkan>::clearColor(
    CommandBufferHandle<GraphicsBackend::Vulkan> cmd,
    const ClearColorValue<GraphicsBackend::Vulkan>& color)
{
    transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        
    VkImageSubresourceRange colorRange = {
        VK_IMAGE_ASPECT_COLOR_BIT,
        0,
        VK_REMAINING_MIP_LEVELS,
        0,
        VK_REMAINING_ARRAY_LAYERS};

    vkCmdClearColorImage(
        cmd,
        getImageHandle(),
        getImageLayout(),
        &color,
        1,
        &colorRange);
}

template <>
void Image<GraphicsBackend::Vulkan>::clearDepthStencil(
    CommandBufferHandle<GraphicsBackend::Vulkan> cmd,
    const ClearDepthStencilValue<GraphicsBackend::Vulkan>& depthStencil)
{
    transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkImageSubresourceRange depthStencilRange = {
        VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,
        0,
        VK_REMAINING_MIP_LEVELS,
        0,
        VK_REMAINING_ARRAY_LAYERS};

    vkCmdClearDepthStencilImage(
        cmd,
        getImageHandle(),
        getImageLayout(),
        &depthStencil,
        1,
        &depthStencilRange);
}

template <>
Image<GraphicsBackend::Vulkan>::Image(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    std::tuple<
        ImageCreateDesc<GraphicsBackend::Vulkan>,
        ImageHandle<GraphicsBackend::Vulkan>,
        AllocationHandle<GraphicsBackend::Vulkan>,
        ImageLayout<GraphicsBackend::Vulkan>>&& descAndData)
: DeviceResource<GraphicsBackend::Vulkan>(
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
Image<GraphicsBackend::Vulkan>::Image(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    ImageCreateDesc<GraphicsBackend::Vulkan>&& desc)
: Image(
    deviceContext,
    std::tuple_cat(
        std::make_tuple(std::move(desc)),
        createImage2D(
            deviceContext->getAllocator(),
            desc.extent.width,
            desc.extent.height,
            desc.format, 
            VK_IMAGE_TILING_OPTIMAL,
            desc.usage,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            desc.name.c_str()),
        std::make_tuple(VK_IMAGE_LAYOUT_UNDEFINED)))
{
}

template <>
Image<GraphicsBackend::Vulkan>::Image(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    const std::shared_ptr<CommandContext<GraphicsBackend::Vulkan>>& commandContext,
    std::tuple<
        ImageCreateDesc<GraphicsBackend::Vulkan>,
        BufferHandle<GraphicsBackend::Vulkan>,
        AllocationHandle<GraphicsBackend::Vulkan>>&& descAndInitialData)
: Image(
    deviceContext,
    std::tuple_cat(
        std::make_tuple(std::move(std::get<0>(descAndInitialData))),
        createImage2D(
            commandContext->commands(),
            deviceContext->getAllocator(),
            std::get<1>(descAndInitialData),
            std::get<0>(descAndInitialData).extent.width,
            std::get<0>(descAndInitialData).extent.height,
            std::get<0>(descAndInitialData).format, 
            VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_LAYOUT_GENERAL,
            std::get<0>(descAndInitialData).usage,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            std::get<0>(descAndInitialData).name.c_str()),
        std::make_tuple(VK_IMAGE_LAYOUT_GENERAL)))
{   
    commandContext->addSubmitFinishedCallback([deviceContext, descAndInitialData](uint64_t){
        vmaDestroyBuffer(deviceContext->getAllocator(), std::get<1>(descAndInitialData), std::get<2>(descAndInitialData));
    });
}

template <>
Image<GraphicsBackend::Vulkan>::Image(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    const std::shared_ptr<CommandContext<GraphicsBackend::Vulkan>>& commandContext,
    const std::filesystem::path& imageFile)
: Image(deviceContext, commandContext, image::load(imageFile, deviceContext))
{
}

template <>
Image<GraphicsBackend::Vulkan>::~Image()
{
    if (auto image = getImageHandle(); image)
        getDeviceContext()->addTimelineCallback(
            [allocator = getDeviceContext()->getAllocator(), image, imageMemory = getImageMemory()](uint64_t){
                vmaDestroyImage(allocator, image, imageMemory);
        });
}

template <>
ImageView<GraphicsBackend::Vulkan>::ImageView(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    ImageViewHandle<GraphicsBackend::Vulkan>&& imageView)
: DeviceResource<GraphicsBackend::Vulkan>(
    deviceContext,
    {"_View"},
    1,
    VK_OBJECT_TYPE_IMAGE_VIEW,
    reinterpret_cast<uint64_t*>(&imageView))
, myImageView(std::move(imageView))
{
}

template <>
ImageView<GraphicsBackend::Vulkan>::ImageView(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    const Image<GraphicsBackend::Vulkan>& image,
    Flags<GraphicsBackend::Vulkan> aspectFlags)
: ImageView<GraphicsBackend::Vulkan>(
    deviceContext,
    createImageView2D(
        deviceContext->getDevice(),
        0, // "reserved for future use"
        image.getImageHandle(),
        image.getDesc().format,
        aspectFlags))
{
}

template <>
ImageView<GraphicsBackend::Vulkan>::~ImageView()
{
    if (auto imageView = getImageViewHandle(); imageView)
        getDeviceContext()->addTimelineCallback(
            [device = getDeviceContext()->getDevice(), imageView](uint64_t){
                vkDestroyImageView(device, imageView, nullptr);
        });
}
