#include "texture.h"
#include "file.h"
#include "vk-utils.h"

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

#include <Tracy.hpp>


template <>
uint32_t Texture<GraphicsBackend::Vulkan>::ourDebugCount = 0;

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

TextureCreateDesc<GraphicsBackend::Vulkan> load(
    const std::filesystem::path& textureFile,
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext)
{
    TextureCreateDesc<GraphicsBackend::Vulkan> desc = {};
    desc.deviceContext = deviceContext;
    desc.debugName = textureFile.u8string();

    auto loadPBin = [&desc](std::istream& stream) {
        cereal::PortableBinaryInputArchive pbin(stream);
        pbin(desc.extent.width, desc.extent.height, desc.channelCount, desc.size);

        std::string debugString;
        debugString.append(desc.debugName);
        debugString.append("_staging");
        
        std::tie(desc.initialData, desc.initialDataMemory) = createBuffer(
            desc.deviceContext->getAllocator(), desc.size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            debugString.c_str());

        std::byte* data;
        CHECK_VK(vmaMapMemory(desc.deviceContext->getAllocator(), desc.initialDataMemory, (void**)&data));
        pbin(cereal::binary_data(data, desc.size));
        vmaUnmapMemory(desc.deviceContext->getAllocator(), desc.initialDataMemory);
    };

    auto savePBin = [&desc](std::ostream& stream) {
        cereal::PortableBinaryOutputArchive pbin(stream);
        pbin(desc.extent.width, desc.extent.height, desc.channelCount, desc.size);

        std::byte* data;
        CHECK_VK(vmaMapMemory(desc.deviceContext->getAllocator(), desc.initialDataMemory, (void**)&data));
        pbin(cereal::binary_data(data, desc.size));
        vmaUnmapMemory(desc.deviceContext->getAllocator(), desc.initialDataMemory);
    };

    auto loadImage = [&desc](std::istream& stream) {
        stbi_io_callbacks callbacks;
        callbacks.read = &stbi_istream_callbacks::read;
        callbacks.skip = &stbi_istream_callbacks::skip;
        callbacks.eof = &stbi_istream_callbacks::eof;
        stbi_uc* stbiImageData =
            stbi_load_from_callbacks(&callbacks, &stream, (int*)&desc.extent.width, (int*)&desc.extent.height, (int*)&desc.channelCount, STBI_rgb_alpha);

        bool hasAlpha = desc.channelCount == 4;
        uint32_t compressedBlockSize = hasAlpha ? 16 : 8;
        desc.size = hasAlpha ? desc.extent.width * desc.extent.height : desc.extent.width * desc.extent.height / 2;

        std::string debugString;
        debugString.append(desc.debugName);
        debugString.append("_staging");
        
        std::tie(desc.initialData, desc.initialDataMemory) = createBuffer(
            desc.deviceContext->getAllocator(), desc.size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            debugString.c_str());

        std::byte* data;
        CHECK_VK(vmaMapMemory(desc.deviceContext->getAllocator(), desc.initialDataMemory, (void**)&data));

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

        vmaUnmapMemory(desc.deviceContext->getAllocator(), desc.initialDataMemory);
        stbi_image_free(stbiImageData);
    };

    loadCachedSourceFile(
        textureFile, textureFile, "stb_image|stb_dxt", "2.20|1.08b", loadImage, loadPBin, savePBin);

    desc.format = desc.channelCount == 3 ? VK_FORMAT_BC1_RGB_UNORM_BLOCK : VK_FORMAT_BC5_UNORM_BLOCK; // todo: write utility function for this
    desc.usage = VK_IMAGE_USAGE_SAMPLED_BIT;

    uint32_t pixelSizeBytesDivisor;
    if (desc.initialData == VK_NULL_HANDLE ||
        desc.size != desc.extent.width * desc.extent.height * getFormatSize(desc.format, pixelSizeBytesDivisor) / pixelSizeBytesDivisor)
        throw std::runtime_error("Failed to load image.");

    return desc;
}

}

template <>
void Texture<GraphicsBackend::Vulkan>::deleteInitialData()
{
    ZoneScopedN("deleteInitialData");

    vmaDestroyBuffer(myDesc.deviceContext->getAllocator(), myDesc.initialData, myDesc.initialDataMemory);
    myDesc.initialData = 0;
    myDesc.initialDataMemory = 0;
}

template <>
ImageViewHandle<GraphicsBackend::Vulkan>
Texture<GraphicsBackend::Vulkan>::createView(Flags<GraphicsBackend::Vulkan> aspectFlags) const
{
    return createImageView2D(myDesc.deviceContext->getDevice(), myImage, myDesc.format, aspectFlags);
}

template <>
void Texture<GraphicsBackend::Vulkan>::transition(
    CommandBufferHandle<GraphicsBackend::Vulkan> commands,
    ImageLayout<GraphicsBackend::Vulkan> layout)
{
    if (myImageLayout != layout)
    {
        transitionImageLayout(commands, myImage, myDesc.format, myImageLayout, layout);
        myImageLayout = layout;
    }
}

template <>
Texture<GraphicsBackend::Vulkan>::Texture(
    TextureCreateDesc<GraphicsBackend::Vulkan>&& desc,
    CommandContext<GraphicsBackend::Vulkan>& commandContext)
    : myDesc(std::move(desc))
    , myImageLayout(VK_IMAGE_LAYOUT_GENERAL)
{
    ZoneScopedN("Texture()");

    ++ourDebugCount;

    std::tie(myImage, myImageMemory) = createImage2D(
        commandContext.commands(),
        myDesc.deviceContext->getAllocator(),
        myDesc.initialData,
        myDesc.extent.width,
        myDesc.extent.height,
        myDesc.format, 
        VK_IMAGE_TILING_OPTIMAL,
        myImageLayout,
        myDesc.usage,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        myDesc.debugName.c_str());
    
    if (myDesc.initialData)
        commandContext.addGarbageCollectCallback([this](uint64_t){ deleteInitialData(); });
}

template <>
Texture<GraphicsBackend::Vulkan>::Texture(
    const std::filesystem::path& textureFile,
    CommandContext<GraphicsBackend::Vulkan>& commandContext)
    : Texture(texture::load(textureFile, commandContext.getDesc().deviceContext), commandContext)
{
    ZoneScopedN("Texture()");
}

template <>
Texture<GraphicsBackend::Vulkan>::~Texture()
{
    ZoneScopedN("~Texture()");

    assert(!myDesc.initialData);
    
    // todo: put on command context delete queue?
    vmaDestroyImage(myDesc.deviceContext->getAllocator(), myImage, myImageMemory);

    --ourDebugCount;
}
