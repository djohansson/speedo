#include "texture.h"

#include "file.h"

#include <cstdint>
#include <iostream>
#include <string>

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

namespace texture
{

TextureCreateDesc<GraphicsBackend::Vulkan>
load(
    const std::filesystem::path& textureFile,
    AllocatorHandle<GraphicsBackend::Vulkan> allocator)
{
    TextureCreateDesc<GraphicsBackend::Vulkan> desc = {};
    desc.debugName = textureFile.u8string();

    auto loadPBin = [&desc, allocator](std::istream& stream) {
        cereal::PortableBinaryInputArchive pbin(stream);
        pbin(desc.nx, desc.ny, desc.nChannels, desc.size);

        std::string debugString;
        debugString.append(desc.debugName);
        debugString.append("_staging");
        
        std::tie(desc.initialData, desc.initialDataMemory) = createBuffer(
            allocator, desc.size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            debugString.c_str());

        std::byte* data;
        CHECK_VK(vmaMapMemory(allocator, desc.initialDataMemory, (void**)&data));
        pbin(cereal::binary_data(data, desc.size));
        vmaUnmapMemory(allocator, desc.initialDataMemory);
    };

    auto savePBin = [&desc, allocator](std::ostream& stream) {
        cereal::PortableBinaryOutputArchive pbin(stream);
        pbin(desc.nx, desc.ny, desc.nChannels, desc.size);

        std::byte* data;
        CHECK_VK(vmaMapMemory(allocator, desc.initialDataMemory, (void**)&data));
        pbin(cereal::binary_data(data, desc.size));
        vmaUnmapMemory(allocator, desc.initialDataMemory);
    };

    auto loadImage = [&desc, allocator](std::istream& stream) {
        stbi_io_callbacks callbacks;
        callbacks.read = &stbi_istream_callbacks::read;
        callbacks.skip = &stbi_istream_callbacks::skip;
        callbacks.eof = &stbi_istream_callbacks::eof;
        stbi_uc* stbiImageData =
            stbi_load_from_callbacks(&callbacks, &stream, (int*)&desc.nx, (int*)&desc.ny, (int*)&desc.nChannels, STBI_rgb_alpha);

        bool hasAlpha = desc.nChannels == 4;
        uint32_t compressedBlockSize = hasAlpha ? 16 : 8;
        desc.size = hasAlpha ? desc.nx * desc.ny : desc.nx * desc.ny / 2;

        std::string debugString;
        debugString.append(desc.debugName);
        debugString.append("_staging");
        
        std::tie(desc.initialData, desc.initialDataMemory) = createBuffer(
            allocator, desc.size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            debugString.c_str());

        std::byte* data;
        CHECK_VK(vmaMapMemory(allocator, desc.initialDataMemory, (void**)&data));

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
        for (uint32_t rowIt = 0; rowIt < desc.ny; rowIt += 4)
        {
            for (uint32_t colIt = 0; colIt < desc.nx; colIt += 4)
            {
                uint32_t offset = (rowIt * desc.nx + colIt) * 4;
                extractBlock(src + offset, desc.nx, 4, block);
                stb_compress_dxt_block(dst, block, hasAlpha, STB_DXT_HIGHQUAL);
                dst += compressedBlockSize;
            }
        }

        vmaUnmapMemory(allocator, desc.initialDataMemory);
        stbi_image_free(stbiImageData);
    };

    loadCachedSourceFile(
        textureFile, textureFile, "stb_image|stb_dxt", "2.20|1.08b", loadImage, loadPBin, savePBin);

    desc.format = desc.nChannels == 3 ? VK_FORMAT_BC1_RGB_UNORM_BLOCK : VK_FORMAT_BC5_UNORM_BLOCK; // todo: write utility function for this
    desc.usage = VK_IMAGE_USAGE_SAMPLED_BIT;

    uint32_t pixelSizeBytesDivisor;
    if (desc.initialData == VK_NULL_HANDLE ||
        desc.size != desc.nx * desc.ny * getFormatSize(desc.format, pixelSizeBytesDivisor) / pixelSizeBytesDivisor)
        throw std::runtime_error("Failed to load image.");

    return desc;
}

}

template <>
Texture<GraphicsBackend::Vulkan>::Texture(
    TextureCreateDesc<GraphicsBackend::Vulkan>&& desc,
    DeviceHandle<GraphicsBackend::Vulkan> device,
    CommandPoolHandle<GraphicsBackend::Vulkan> commandPool,
    QueueHandle<GraphicsBackend::Vulkan> queue,
    AllocatorHandle<GraphicsBackend::Vulkan> allocator)
    : myDesc(std::move(desc))
    , myDevice(device)
    , myAllocator(allocator)
{
    std::tie(myImage, myImageMemory) = createImage2D(
        myDevice,
        commandPool,
        queue,
        myAllocator,
        myDesc.initialData,
        myDesc.nx,
        myDesc.ny,
        myDesc.format, 
        VK_IMAGE_TILING_OPTIMAL,
        hasDepthComponent(myDesc.format) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        myDesc.usage,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        myDesc.debugName.c_str());

    vmaDestroyBuffer(allocator, desc.initialData, desc.initialDataMemory);
}

template <>
Texture<GraphicsBackend::Vulkan>::Texture(
    const std::filesystem::path& textureFile,
    DeviceHandle<GraphicsBackend::Vulkan> device,
    CommandPoolHandle<GraphicsBackend::Vulkan> commandPool,
    QueueHandle<GraphicsBackend::Vulkan> queue,
    AllocatorHandle<GraphicsBackend::Vulkan> allocator)
    : Texture(texture::load(textureFile, allocator), device, commandPool, queue, allocator)
{
}

template <>
Texture<GraphicsBackend::Vulkan>::~Texture()
{
    vmaDestroyImage(myAllocator, myImage, myImageMemory);
}

template <>
ImageViewHandle<GraphicsBackend::Vulkan>
Texture<GraphicsBackend::Vulkan>::createView(Flags<GraphicsBackend::Vulkan> aspectFlags) const
{
    return createImageView2D(myDevice, myImage, myDesc.format, aspectFlags);
}
