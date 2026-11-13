// the Metal backend's images: textures (2D, or 3D for a volume), in private storage unless the host sees them (shared),
// filled by the blit encoder from a buffer, level by level; Metal has no layouts (an image's is kept for rhi's
// bookkeeping: Transition records it), and views are texture views (a format, levels, a swizzle). Clears and scaled
// blits come with the render passes (step 5), placement in a memory block with heaps
#include <rhi/metal/backend.h>

#include <rhi/buffer.h>
#include <rhi/device.h>
#include <rhi/image.h>
#include <rhi/rhi.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <algorithm>
#include <memory>
#include <string_view>
#include <tuple>
#include <utility>

namespace rhi
{

namespace metal
{

MTL::PixelFormat ToMetal(Format format)
{
	switch (format)
	{
	case Format::kUndefined: return MTL::PixelFormatInvalid;
	case Format::kR8G8B8A8Unorm: return MTL::PixelFormatRGBA8Unorm;
	case Format::kR8G8B8A8Srgb: return MTL::PixelFormatRGBA8Unorm_sRGB;
	case Format::kB8G8R8A8Unorm: return MTL::PixelFormatBGRA8Unorm;
	case Format::kB8G8R8A8Srgb: return MTL::PixelFormatBGRA8Unorm_sRGB;
	// Vulkan names a packed format's components from its top bits, Metal from its bottom ones
	case Format::kA2R10G10B10UnormPack32: return MTL::PixelFormatBGR10A2Unorm;
	case Format::kA2B10G10R10UnormPack32: return MTL::PixelFormatRGB10A2Unorm;
	case Format::kR16G16B16A16Sfloat: return MTL::PixelFormatRGBA16Float;
	case Format::kR16Sfloat: return MTL::PixelFormatR16Float;
	case Format::kD32Sfloat: return MTL::PixelFormatDepth32Float;
	case Format::kD32SfloatS8Uint: return MTL::PixelFormatDepth32Float_Stencil8;
	// Metal's BC1 has the punch through alpha Vulkan's RGB variant ignores (a block using it decodes alpha 0, not 1)
	case Format::kBC1RgbUnorm: return MTL::PixelFormatBC1_RGBA;
	case Format::kBC1RgbSrgb: return MTL::PixelFormatBC1_RGBA_sRGB;
	case Format::kBC3Unorm: return MTL::PixelFormatBC3_RGBA;
	case Format::kBC3Srgb: return MTL::PixelFormatBC3_RGBA_sRGB;
	case Format::kBC4Unorm: return MTL::PixelFormatBC4_RUnorm;
	case Format::kBC5Unorm: return MTL::PixelFormatBC5_RGUnorm;
	case Format::kBC7Unorm: return MTL::PixelFormatBC7_RGBAUnorm;
	case Format::kBC7Srgb: return MTL::PixelFormatBC7_RGBAUnorm_sRGB;
	case Format::kR32Sfloat: return MTL::PixelFormatR32Float;
	case Format::kR32G32B32A32Sfloat: return MTL::PixelFormatRGBA32Float;
	case Format::kR16G16Sfloat: return MTL::PixelFormatRG16Float;
	case Format::kR32G32Sfloat: return MTL::PixelFormatRG32Float;
	case Format::kR32G32Uint: return MTL::PixelFormatRG32Uint;
	case Format::kR8G8B8Unorm:
	case Format::kB8G8R8Unorm:
	case Format::kD24UnormS8Uint:
		break;
	}
	ENSUREF(false, "format {} has no Metal pixel format (24 bit color, or depth 24 stencil 8 on Apple's gpus)", std::to_underlying(format));
	return MTL::PixelFormatInvalid;
}

} // namespace metal

IMPLEMENT_OBJECT_GETINSTANCE(Image<kMetal>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(Image<kMetal>);
IMPLEMENT_OBJECT_GETINSTANCE(ImageView<kMetal>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(ImageView<kMetal>);

namespace image
{

// a texture descriptor for desc (released by the caller). every texture may be viewed in another format (an sRGB view of
// a unorm image, a depth stencil image's stencil), which costs some gpus' lossless compression
static MTL::TextureDescriptor* Descriptor(const ImageCreateDesc<kMetal>& desc)
{
	auto* descriptor = MTL::TextureDescriptor::alloc()->init();
	descriptor->setTextureType(desc.extent.depth > 1 ? MTL::TextureType3D : MTL::TextureType2D);
	descriptor->setPixelFormat(metal::ToMetal(desc.format));
	descriptor->setWidth(desc.extent.width);
	descriptor->setHeight(desc.extent.height);
	descriptor->setDepth(std::max(desc.extent.depth, 1U));
	descriptor->setMipmapLevelCount(desc.levelCount);
	MTL::TextureUsage usage = MTL::TextureUsagePixelFormatView;
	if (Any(desc.usageFlags & ImageUsage::kSampled))
		usage |= MTL::TextureUsageShaderRead;
	if (Any(desc.usageFlags & ImageUsage::kStorage))
		usage |= MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite;
	if (Any(desc.usageFlags & (ImageUsage::kColorAttachment | ImageUsage::kDepthStencilAttachment)))
		usage |= MTL::TextureUsageRenderTarget;
	descriptor->setUsage(usage);
	const bool hostVisible = (desc.memoryFlags & MemoryProperty::kHostVisible) == MemoryProperty::kHostVisible;
	descriptor->setStorageMode(hostVisible ? MTL::StorageModeShared : MTL::StorageModePrivate);
	return descriptor;
}

static MTL::Texture* CreateTexture(const ImageCreateDesc<kMetal>& desc)
{
	auto* descriptor = Descriptor(desc);
	MTL::Texture* texture = desc.device->newTexture(descriptor);
	descriptor->release();
	ENSUREF(texture != nullptr, "{}: Failed to create a Metal texture", GetDebugName(desc));
	texture->setLabel(NS::String::string(desc.name.c_str(), NS::UTF8StringEncoding));
	return texture;
}

// a texture of desc whose levels are copied from buffer (byteCount bytes), where desc's levelOffsets have them (else
// packed), by the command buffer's blit encoder
static MTL::Texture* CreateUploadedTexture(
	metal::CommandBufferObject& cmd, MTL::Buffer* buffer, uint64_t byteCount, const ImageCreateDesc<kMetal>& desc)
{
	ENSURE(buffer != nullptr);
	ENSUREF(
		desc.levelOffsets.empty() || desc.levelOffsets.size() == desc.levelCount,
		"{}: an upload of {} levels was given {} offsets",
		GetDebugName(desc),
		desc.levelCount,
		desc.levelOffsets.size());
	const auto block = BlockOf(desc.format);
	ENSUREF(
		block.byteCount > 0,
		"{}: format {} can't be uploaded (its texels aren't one run of bytes)",
		GetDebugName(desc),
		std::to_underlying(desc.format));

	auto* texture = CreateTexture(desc);
	auto* blit = cmd.BlitEncoder();
	uint64_t packedOffset = 0;
	for (uint32_t level = 0; level < desc.levelCount; level++)
	{
		const auto extent = desc.LevelExtent(level);
		const auto levelByteCount = PackedByteCount(desc.format, extent);
		const auto offset = desc.levelOffsets.empty() ? packedOffset : desc.levelOffsets[level];
		ENSUREF(
			offset + levelByteCount <= byteCount,
			"{}: level {} ({} bytes from {}) runs past the source's {} bytes",
			GetDebugName(desc),
			level,
			levelByteCount,
			offset,
			byteCount);
		packedOffset += levelByteCount;
		// a row of blocks, and for a volume a slice (Metal takes 0 for a 2D texture's)
		const uint64_t bytesPerRow = uint64_t{(extent.width + block.width - 1) / block.width} * block.byteCount;
		const uint64_t bytesPerImage = extent.depth > 1 ? bytesPerRow * ((extent.height + block.height - 1) / block.height) : 0;
		blit->copyFromBuffer(
			buffer,
			offset,
			bytesPerRow,
			bytesPerImage,
			MTL::Size(extent.width, extent.height, extent.depth),
			texture,
			0,
			level,
			MTL::Origin(0, 0, 0));
	}
	return texture;
}

[[noreturn]] static void NotYet(std::string_view what)
{
	ENSUREF(false, "the Metal backend's images: {} come with its render passes (docs/backends-plan.md, step 5)", what);
	__builtin_trap();
}

} // namespace image

template <>
void Image<kMetal>::Transition(CommandBufferHandle<kMetal> /*cmd*/, ImageLayout layout, ImageAspect aspectFlags)
{
	// no layouts in Metal (and the resources are hazard tracked): kept for rhi's bookkeeping alone
	if (aspectFlags == ImageAspect::kNone)
		aspectFlags = AspectOf(GetDesc().format);
	InternalSetImageLayout(layout);
	InternalSetAspectFlags(aspectFlags);
}

template <>
void Image<kMetal>::BlitFrom(CommandBufferHandle<kMetal> /*cmd*/, const Image& /*source*/)
{
	image::NotYet("scaled blits");
}

template <>
void Image<kMetal>::GenerateMips(CommandBufferHandle<kMetal> cmd, ImageLayout layout)
{
	ZoneScopedN("Image::GenerateMips");

	ENSURE(GetDesc().layout == ImageLayout::kTransferDestination);
	if (GetDesc().levelCount > 1)
		cmd->BlitEncoder()->generateMipmaps(std::get<0>(myImage));
	InternalSetImageLayout(layout);
	InternalSetAspectFlags(ImageAspect::kColor);
}

template <>
void Image<kMetal>::Clear(CommandBufferHandle<kMetal> /*cmd*/, const ClearValue& /*value*/, const std::optional<ImageSubresourceRange<kMetal>>& /*range*/)
{
	image::NotYet("clears");
}

template <>
Image<kMetal>::Image(CreateDescType&& desc, ValueType&& data)
	: DeviceObject<Image<kMetal>>(std::forward<CreateDescType>(desc))
	, myImage(std::forward<ValueType>(data))
{}

template <>
Image<kMetal>::Image(CreateDescType&& desc, const MemoryBlock<kMetal>& /*memory*/, uint64_t /*offset*/)
	: DeviceObject<Image<kMetal>>(std::forward<CreateDescType>(desc))
{
	ENSUREF(false, "the Metal backend places no images in memory blocks yet (heaps)");
}

template <>
MemoryRequirements Image<kMetal>::GetMemoryRequirements(const Device<kMetal>& device, const CreateDescType& desc)
{
	auto* descriptor = image::Descriptor(desc);
	auto sizeAndAlign = static_cast<MTL::Device*>(device)->heapTextureSizeAndAlign(descriptor);
	descriptor->release();
	return {.size = sizeAndAlign.size, .alignment = sizeAndAlign.align, .memoryTypeBits = ~0U};
}

template <>
Image<kMetal>::Image(CreateDescType&& desc)
	: Image(std::forward<CreateDescType>(desc), ValueType{image::CreateTexture(desc), nullptr})
{}

template <>
Image<kMetal>::Image(
	CreateDescType&& desc,
	CommandBufferHandle<kMetal> cmd,
	core::TaskCreateInfo<void>& timlineCallbackOut,
	std::tuple<BufferHandle<kMetal>, AllocationHandle<kMetal>>&& initialData,
	uint64_t byteCount)
	: Image(
		std::forward<CreateDescType>(desc),
		ValueType{image::CreateUploadedTexture(*cmd, std::get<0>(initialData), byteCount, desc), nullptr})
{
	InternalSetImageLayout(ImageLayout::kTransferDestination); // as the upload leaves it
	timlineCallbackOut = core::CreateTask([buffer = std::get<0>(initialData)] { buffer->release(); });
}

template <>
Image<kMetal>::Image(
	CreateDescType&& desc,
	CommandBufferHandle<kMetal> cmd,
	const void* initialData,
	size_t initialDataSize,
	core::TaskCreateInfo<void>& timlineCallbackOut)
	: Image(
		std::forward<CreateDescType>(desc),
		cmd,
		timlineCallbackOut,
		// a staging buffer of the data (shared storage), released once the upload's commands are done
		[&desc, initialData, initialDataSize]
		{
			MTL::Buffer* staging = desc.device->newBuffer(initialData, initialDataSize, MTL::ResourceStorageModeShared);
			ENSUREF(staging != nullptr, "{}: Failed to create a Metal staging buffer of {} bytes", GetDebugName(desc), initialDataSize);
			return std::tuple<BufferHandle<kMetal>, AllocationHandle<kMetal>>{staging, nullptr};
		}(),
		initialDataSize)
{}

template <>
Image<kMetal>::Image(
	CreateDescType&& desc, Buffer<kMetal>&& staging, CommandBufferHandle<kMetal> cmd, core::TaskCreateInfo<void>& timlineCallbackOut)
	: Image(
		std::forward<CreateDescType>(desc),
		ValueType{image::CreateUploadedTexture(*cmd, staging.GetBuffer(), staging.GetDesc().size, desc), nullptr})
{
	InternalSetImageLayout(ImageLayout::kTransferDestination); // as the upload leaves it
	timlineCallbackOut = core::CreateTask([staging = std::make_shared<Buffer<kMetal>>(std::move(staging))] {});
}

template <>
Image<kMetal>::~Image()
{
	if (IsValid() && std::get<0>(myImage) != nullptr)
		std::get<0>(myImage)->release();
}

template <>
ImageView<kMetal>::ImageView(CreateDescType&& desc, ImageViewHandle<kMetal>&& view)
	: DeviceObject<ImageView<kMetal>>(std::forward<CreateDescType>(desc))
	, myView(std::forward<ImageViewHandle<kMetal>>(view))
{}

namespace image
{

static MTL::TextureSwizzle ToMetal(ComponentSwizzle swizzle, MTL::TextureSwizzle identity) noexcept
{
	switch (swizzle)
	{
	case ComponentSwizzle::kIdentity: return identity;
	case ComponentSwizzle::kZero: return MTL::TextureSwizzleZero;
	case ComponentSwizzle::kOne: return MTL::TextureSwizzleOne;
	case ComponentSwizzle::kR: return MTL::TextureSwizzleRed;
	case ComponentSwizzle::kG: return MTL::TextureSwizzleGreen;
	case ComponentSwizzle::kB: return MTL::TextureSwizzleBlue;
	case ComponentSwizzle::kA: return MTL::TextureSwizzleAlpha;
	}
	return identity;
}

} // namespace image

template <>
ImageView<kMetal>::ImageView(CreateDescType&& desc)
	: ImageView(
		std::forward<CreateDescType>(desc),
		[&desc]
		{
			auto* source = desc.image.handle;
			ENSUREF(source != nullptr, "{}: a view of no image", GetDebugName(desc));
			const auto levelCount = desc.levelCount == 0 ? source->mipmapLevelCount() - desc.baseLevel : desc.levelCount;
			// a depth stencil image's stencil is a format of its own in Metal
			auto format = metal::ToMetal(desc.format);
			if (desc.aspectFlags == ImageAspect::kStencil && format == MTL::PixelFormatDepth32Float_Stencil8)
				format = MTL::PixelFormatX32_Stencil8;
			const MTL::TextureSwizzleChannels swizzle(
				image::ToMetal(desc.components[0], MTL::TextureSwizzleRed),
				image::ToMetal(desc.components[1], MTL::TextureSwizzleGreen),
				image::ToMetal(desc.components[2], MTL::TextureSwizzleBlue),
				image::ToMetal(desc.components[3], MTL::TextureSwizzleAlpha));
			MTL::Texture* texture = source->newTextureView(
				format,
				desc.image.volume ? MTL::TextureType3D : MTL::TextureType2D,
				NS::Range(desc.baseLevel, levelCount),
				NS::Range(0, 1),
				swizzle);
			ENSUREF(texture != nullptr, "{}: Failed to create a Metal texture view", GetDebugName(desc));
			texture->setLabel(NS::String::string(desc.name.c_str(), NS::UTF8StringEncoding));
			return new metal::ImageViewObject{.texture = texture};
		}())
{}

template <>
ImageView<kMetal>::~ImageView()
{
	if (!IsValid() || myView == nullptr)
		return;
	myView->texture->release();
	delete myView;
}

} // namespace rhi
