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

Texture<GraphicsBackend::Vulkan>::TextureData load(const std::filesystem::path& textureFile)
{
    Texture<GraphicsBackend::Vulkan>::TextureData data = {};
    data.debugName = textureFile.u8string();

    auto loadPBin = [&data](std::istream& stream) {
        cereal::PortableBinaryInputArchive pbin(stream);
        pbin(data.nx, data.ny, data.nChannels, data.imageSize);
        data.imageData = std::make_unique<std::byte[]>(data.imageSize);
        pbin(cereal::binary_data(data.imageData.get(), data.imageSize));
    };

    auto savePBin = [&data](std::ostream& stream) {
        cereal::PortableBinaryOutputArchive pbin(stream);
        pbin(data.nx, data.ny, data.nChannels, data.imageSize);
        pbin(cereal::binary_data(data.imageData.get(), data.imageSize));
    };

    auto loadImage = [&data](std::istream& stream) {
        stbi_io_callbacks callbacks;
        callbacks.read = &stbi_istream_callbacks::read;
        callbacks.skip = &stbi_istream_callbacks::skip;
        callbacks.eof = &stbi_istream_callbacks::eof;
        stbi_uc* stbiImageData =
            stbi_load_from_callbacks(&callbacks, &stream, (int*)&data.nx, (int*)&data.ny, (int*)&data.nChannels, STBI_rgb_alpha);

        bool hasAlpha = data.nChannels == 4;
        uint32_t compressedBlockSize = hasAlpha ? 16 : 8;
        data.imageSize = hasAlpha ? data.nx * data.ny : data.nx * data.ny / 2;
        data.imageData = std::make_unique<std::byte[]>(data.imageSize);

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
        stbi_uc* dst = reinterpret_cast<stbi_uc*>(data.imageData.get());
        for (uint32_t rowIt = 0; rowIt < data.ny; rowIt += 4)
        {
            for (uint32_t colIt = 0; colIt < data.nx; colIt += 4)
            {
                uint32_t offset = (rowIt * data.nx + colIt) * 4;
                extractBlock(src + offset, data.nx, 4, block);
                stb_compress_dxt_block(dst, block, hasAlpha, STB_DXT_HIGHQUAL);
                dst += compressedBlockSize;
            }
        }

        stbi_image_free(stbiImageData);
    };

    loadCachedSourceFile(
        textureFile, textureFile, "stb_image|stb_dxt", "2.20|1.08b", loadImage, loadPBin, savePBin);

    if (data.imageData == nullptr)
        throw std::runtime_error("Failed to load image.");

    data.format = data.nChannels == 3 ? VK_FORMAT_BC1_RGB_UNORM_BLOCK : VK_FORMAT_BC5_UNORM_BLOCK; // todo: write utility function for this
    data.flags = VK_IMAGE_USAGE_SAMPLED_BIT;
    data.aspectFlags = VK_IMAGE_ASPECT_COLOR_BIT;

    return data;
}

}

template <>
Texture<GraphicsBackend::Vulkan>::Texture(
    VkDevice device, VkCommandPool commandPool, VkQueue queue, VmaAllocator allocator,
    const TextureData& data)
    : device(device)
    , allocator(allocator)
    , format(data.format)
{
    std::tie(image, imageMemory) = createDeviceLocalImage2D(
        device, commandPool, queue, allocator,
        data.imageData.get(), data.nx, data.ny, data.format, data.flags,
        data.debugName.c_str());

    imageView = createImageView2D(device, image, data.format, data.aspectFlags);
}

template <>
Texture<GraphicsBackend::Vulkan>::Texture(
    VkDevice device, VkCommandPool commandPool, VkQueue queue, VmaAllocator allocator,
    const std::filesystem::path& textureFile)
    : Texture(device, commandPool, queue, allocator, texture::load(textureFile))
{
}

template <>
Texture<GraphicsBackend::Vulkan>::~Texture()
{
    vmaDestroyImage(allocator, image, imageMemory);
    vkDestroyImageView(device, imageView, nullptr);
}
