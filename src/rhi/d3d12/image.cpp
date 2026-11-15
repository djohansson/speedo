// the D3D12 backend's images: textures (2D, or 3D for a volume) from D3D12MA, device local, a depth format's created
// typeless so that both its depth stencil view and a shader's view of it can be made, a color format's castable to its
// sRGB kin (enhanced barriers' castable formats); filled by copies from a buffer, level by level. layouts are D3D12's
// (enhanced barriers): Transition is a texture barrier, its two sides' sync and access those of the layouts (the list's
// type's alone: a compute list has no graphics stages, a copy list copies). a view is its descriptors in the device's cpu
// heaps, those the image's usage allows (shader resource, unordered access, render target, depth stencil), copied where
// a set or a pass takes them
#include <rhi/d3d12/backend.h>

#include <rhi/buffer.h>
#include <rhi/device.h>
#include <rhi/image.h>
#include <rhi/memory.h>
#include <rhi/rhi.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <span>
#include <tuple>
#include <utility>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(Image<kD3D12>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(Image<kD3D12>);
IMPLEMENT_OBJECT_GETINSTANCE(ImageView<kD3D12>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(ImageView<kD3D12>);

namespace image
{

// a depth format's resource format: typeless, so that a shader's view (a color format of its bits) can be made too
static DXGI_FORMAT ResourceFormat(Format format)
{
	switch (format)
	{
	case Format::kD32Sfloat: return DXGI_FORMAT_R32_TYPELESS;
	case Format::kD32SfloatS8Uint: return DXGI_FORMAT_R32G8X24_TYPELESS;
	case Format::kD24UnormS8Uint: return DXGI_FORMAT_R24G8_TYPELESS;
	default: return d3d12::ToD3D12(format);
	}
}

// a shader's view's format of a format and aspect (a depth stencil image's depth or stencil as a color format)
static DXGI_FORMAT ShaderFormat(Format format, ImageAspect aspect)
{
	const bool stencil = aspect == ImageAspect::kStencil;
	switch (format)
	{
	case Format::kD32Sfloat: return DXGI_FORMAT_R32_FLOAT;
	case Format::kD32SfloatS8Uint: return stencil ? DXGI_FORMAT_X32_TYPELESS_G8X24_UINT : DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
	case Format::kD24UnormS8Uint: return stencil ? DXGI_FORMAT_X24_TYPELESS_G8_UINT : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
	default: return d3d12::ToD3D12(format);
	}
}

// the formats a color image's views may take besides its own: its sRGB or unorm kin
static std::vector<DXGI_FORMAT> CastableFormats(Format format)
{
	constexpr std::array kPairs{
		std::pair{Format::kR8G8B8A8Unorm, Format::kR8G8B8A8Srgb},
		std::pair{Format::kB8G8R8A8Unorm, Format::kB8G8R8A8Srgb},
		std::pair{Format::kBC1RgbUnorm, Format::kBC1RgbSrgb},
		std::pair{Format::kBC3Unorm, Format::kBC3Srgb},
		std::pair{Format::kBC7Unorm, Format::kBC7Srgb},
	};
	for (const auto& [unorm, srgb] : kPairs)
		if (format == unorm || format == srgb)
			return {d3d12::ToD3D12(unorm), d3d12::ToD3D12(srgb)};
	return {};
}

// whether a color image is cleared as a render target (Image::Clear: D3D12 clears a render target view, where Vulkan
// clears a transfer destination): a transfer destination's of a format that can be rendered to (not block compressed)
static bool Clearable(const ImageCreateDesc<kD3D12>& desc)
{
	return Any(desc.usageFlags & ImageUsage::kTransferDestination) && BlockOf(desc.format).width == 1 && HasColorComponent(desc.format);
}

static D3D12_RESOURCE_DESC1 ResourceDesc(const ImageCreateDesc<kD3D12>& desc)
{
	auto flags = D3D12_RESOURCE_FLAG_NONE;
	if (Any(desc.usageFlags & ImageUsage::kStorage))
		flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	if (Any(desc.usageFlags & ImageUsage::kColorAttachment) || Clearable(desc))
		flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	if (Any(desc.usageFlags & ImageUsage::kDepthStencilAttachment) || (HasDepthComponent(desc.format) && Any(desc.usageFlags & ImageUsage::kTransferDestination)))
	{
		flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
		if (!Any(desc.usageFlags & ImageUsage::kSampled))
			flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
	}
	const bool volume = desc.extent.depth > 1;
	return D3D12_RESOURCE_DESC1{
		.Dimension = volume ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D,
		.Alignment = 0,
		.Width = desc.extent.width,
		.Height = desc.extent.height,
		.DepthOrArraySize = static_cast<UINT16>(volume ? desc.extent.depth : 1),
		.MipLevels = static_cast<UINT16>(desc.levelCount),
		.Format = ResourceFormat(desc.format),
		.SampleDesc = {.Count = 1, .Quality = 0},
		.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
		.Flags = flags,
		.SamplerFeedbackMipRegion = {},
	};
}

static d3d12::ImageObject* NewObject(const ImageCreateDesc<kD3D12>& desc, const D3D12_RESOURCE_DESC1& resourceDesc)
{
	return new d3d12::ImageObject{ //NOLINT(cppcoreguidelines-owning-memory)
		.format = desc.format,
		.dimension = resourceDesc.Dimension,
		.flags = resourceDesc.Flags,
		.levelCount = desc.levelCount,
		.layout = D3D12_BARRIER_LAYOUT_COMMON};
}

// an image of desc in the default heap, in layout
static d3d12::ImageObject* Create(const ImageCreateDesc<kD3D12>& desc, D3D12_BARRIER_LAYOUT layout)
{
	ENSUREF(
		(desc.memoryFlags & MemoryProperty::kHostVisible) != MemoryProperty::kHostVisible,
		"{}: an image the host sees (the D3D12 backend's images are device local: read them back by a copy)",
		GetDebugName(desc));
	auto& allocator = d3d12::AllocatorObject::ForDevice(desc.device);
	const auto resourceDesc = ResourceDesc(desc);
	const auto castable = CastableFormats(desc.format);
	D3D12MA::ALLOCATION_DESC allocationDesc{};
	allocationDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
	// an attachment committed, a heap of its own (D3D12MA's advice for render targets and depth stencils)
	if ((resourceDesc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0)
		allocationDesc.Flags = D3D12MA::ALLOCATION_FLAG_COMMITTED;
	auto* object = NewObject(desc, resourceDesc);
	d3d12::Check(
		allocator.allocator->CreateResource3(
			&allocationDesc,
			&resourceDesc,
			layout,
			nullptr,
			static_cast<UINT32>(castable.size()),
			castable.empty() ? nullptr : castable.data(),
			&object->allocation,
			IID_NULL,
			nullptr),
		std::format("{}: CreateResource3 (an image)", GetDebugName(desc)));
	object->resource = object->allocation->GetResource();
	object->layout = layout;
	d3d12::SetName(object->resource, desc.name);
	return object;
}

// an image of desc whose levels are copied from buffer (byteCount bytes), where desc's levelOffsets have them (else
// packed), on the command list (the image made in the copy's layout)
static d3d12::ImageObject* CreateUploaded(
	d3d12::CommandBufferObject& cmd, ID3D12Resource* buffer, uint64_t byteCount, const ImageCreateDesc<kD3D12>& desc)
{
	ENSURE(buffer != nullptr);
	ENSUREF(
		desc.levelOffsets.empty() || desc.levelOffsets.size() == desc.levelCount,
		"{}: an upload of {} levels was given {} offsets",
		GetDebugName(desc),
		desc.levelCount,
		desc.levelOffsets.size());
	const auto block = BlockOf(desc.format);
	ENSUREF(block.byteCount > 0, "{}: format {} can't be uploaded (its texels aren't one run of bytes)", GetDebugName(desc), std::to_underlying(desc.format));

	// (a copy list's in the common layout, see TransitionImage)
	auto* object = Create(desc, cmd.type == D3D12_COMMAND_LIST_TYPE_COPY ? D3D12_BARRIER_LAYOUT_COMMON : D3D12_BARRIER_LAYOUT_COPY_DEST);
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
		// rows of blocks packed (the device copies with unrestricted pitches, see commandencoder.cpp)
		const auto rowPitch = static_cast<UINT>(uint64_t{(extent.width + block.width - 1) / block.width} * block.byteCount);
		const D3D12_TEXTURE_COPY_LOCATION source{
			.pResource = buffer,
			.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
			.PlacedFootprint = {
				.Offset = offset,
				.Footprint = {.Format = ResourceFormat(desc.format), .Width = extent.width, .Height = extent.height, .Depth = extent.depth, .RowPitch = rowPitch}}};
		const D3D12_TEXTURE_COPY_LOCATION destination{
			.pResource = object->resource, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = level};
		cmd.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
	}
	return object;
}

// a layout's side of a barrier: the work that uses it, and how, on a list of type (a compute list's shading is compute's,
// a copy list's copies alone)
struct Side
{
	D3D12_BARRIER_SYNC sync = D3D12_BARRIER_SYNC_NONE;
	D3D12_BARRIER_ACCESS access = D3D12_BARRIER_ACCESS_NO_ACCESS;
};

static Side SideOf(D3D12_BARRIER_LAYOUT layout, D3D12_COMMAND_LIST_TYPE type)
{
	const auto shading = type == D3D12_COMMAND_LIST_TYPE_COMPUTE ? D3D12_BARRIER_SYNC_COMPUTE_SHADING : D3D12_BARRIER_SYNC_ALL_SHADING;
	switch (layout)
	{
	case D3D12_BARRIER_LAYOUT_COPY_SOURCE: return {D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_SOURCE};
	case D3D12_BARRIER_LAYOUT_COPY_DEST: return {D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_DEST};
	case D3D12_BARRIER_LAYOUT_SHADER_RESOURCE: return {shading, D3D12_BARRIER_ACCESS_SHADER_RESOURCE};
	case D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS: return {shading, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS};
	case D3D12_BARRIER_LAYOUT_RENDER_TARGET: return {D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET};
	case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE: return {D3D12_BARRIER_SYNC_DEPTH_STENCIL, D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE};
	default: return {}; // undefined, common, present: no work before or after on this list
	}
}

} // namespace image

namespace d3d12
{

// a texture barrier on cmd's list from one layout to another, every subresource (the work and accesses each layout's,
// see SideOf); from an undefined layout the contents are discarded, once last's work is done (a discard without it waits
// for nothing, which the debug layer refuses where the resource was used before: error 1417, a swapchain's image, last a
// present's copy source, which its frame transitions from undefined each frame as Vulkan's swapchain images are)
void Transition(CommandBufferObject& cmd, ID3D12Resource* resource, D3D12_BARRIER_LAYOUT before, D3D12_BARRIER_LAYOUT after, D3D12_BARRIER_LAYOUT last)
{
	if (before == after && before != D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS)
		return;
	// an attachment's contents discarded (from an undefined layout) are initialized where the discard leaves it in its
	// attachment's layout, which a resource not zeroed (placed, or committed not zeroed) needs before its first use (the
	// debug layer's error 1422: the shadow atlas, cleared by rects): on a direct list, through that layout
	if (before == D3D12_BARRIER_LAYOUT_UNDEFINED && cmd.type == D3D12_COMMAND_LIST_TYPE_DIRECT)
	{
		const auto flags = resource->GetDesc().Flags;
		const auto attachment = (flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0 ? D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE
								: (flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0 ? D3D12_BARRIER_LAYOUT_RENDER_TARGET
																						: D3D12_BARRIER_LAYOUT_UNDEFINED;
		if (attachment != D3D12_BARRIER_LAYOUT_UNDEFINED && after != attachment)
		{
			Transition(cmd, resource, before, attachment, last);
			before = attachment;
			last = D3D12_BARRIER_LAYOUT_UNDEFINED;
		}
	}
	// the work before it waits for: its layout's (a discard's, the layout it was last used in); from the common layout (the
	// present's, a copy queue's textures'), all the list's work before, which may have used it without a barrier of its
	// own after (a frame's draws, then its transition to present, then a capture's copy from it: error 1417). a stage the
	// list hasn't is another list's work, which the queues' fences order
	const auto waited = before == D3D12_BARRIER_LAYOUT_UNDEFINED ? last : before;
	auto sync = image::SideOf(waited, cmd.type).sync;
	if (waited == D3D12_BARRIER_LAYOUT_COMMON)
		sync = cmd.type == D3D12_COMMAND_LIST_TYPE_COPY ? D3D12_BARRIER_SYNC_COPY : D3D12_BARRIER_SYNC_ALL;
	if ((cmd.type == D3D12_COMMAND_LIST_TYPE_COMPUTE && (sync == D3D12_BARRIER_SYNC_RENDER_TARGET || sync == D3D12_BARRIER_SYNC_DEPTH_STENCIL)) ||
		(cmd.type == D3D12_COMMAND_LIST_TYPE_COPY && sync != D3D12_BARRIER_SYNC_COPY))
		sync = D3D12_BARRIER_SYNC_NONE;
	// (its accesses its layout's: from the common layout, waiting, the common access, as D3D12 wants a wait with an
	// access unless it discards)
	auto access = before == D3D12_BARRIER_LAYOUT_UNDEFINED ? D3D12_BARRIER_ACCESS_NO_ACCESS : image::SideOf(before, cmd.type).access;
	if (before == D3D12_BARRIER_LAYOUT_COMMON && sync != D3D12_BARRIER_SYNC_NONE)
		access = D3D12_BARRIER_ACCESS_COMMON;
	const image::Side from{.sync = sync, .access = access};
	// the work after that waits for it: its layout's; to the common layout (the present's), all the list's work after,
	// which may use it (a capture's copy after the transition to present: error 1417, an access after a barrier whose
	// SyncAfter is none)
	auto to = image::SideOf(after, cmd.type);
	if (after == D3D12_BARRIER_LAYOUT_COMMON && cmd.type != D3D12_COMMAND_LIST_TYPE_COPY)
		to = {.sync = D3D12_BARRIER_SYNC_ALL, .access = D3D12_BARRIER_ACCESS_COMMON};
	const D3D12_TEXTURE_BARRIER barrier{
		.SyncBefore = from.sync,
		.SyncAfter = to.sync,
		.AccessBefore = from.access,
		.AccessAfter = to.access,
		.LayoutBefore = before,
		.LayoutAfter = after,
		.pResource = resource,
		.Subresources = {.IndexOrFirstMipLevel = 0xFFFFFFFF, .NumMipLevels = 0, .FirstArraySlice = 0, .NumArraySlices = 0, .FirstPlane = 0, .NumPlanes = 0},
		.Flags = before == D3D12_BARRIER_LAYOUT_UNDEFINED ? D3D12_TEXTURE_BARRIER_FLAG_DISCARD : D3D12_TEXTURE_BARRIER_FLAG_NONE};
	const D3D12_BARRIER_GROUP group{.Type = D3D12_BARRIER_TYPE_TEXTURE, .NumBarriers = 1, .pTextureBarriers = &barrier};
	cmd.list->Barrier(1, &group);
}

ImageObject* NewImageObject(const ImageCreateDesc<kD3D12>& desc)
{
	return image::Create(desc, D3D12_BARRIER_LAYOUT_COMMON);
}

void DeleteImageObject(ImageObject* image) noexcept
{
	if (image == nullptr)
		return;
	// the allocation owns its resource; a placed one (no allocation of its own) is the image's
	if (image->allocation != nullptr)
		image->allocation->Release();
	else if (image->resource != nullptr)
		image->resource->Release();
	delete image; //NOLINT(cppcoreguidelines-owning-memory)
}

void TransitionImage(CommandBufferObject& cmd, ImageObject& image, D3D12_BARRIER_LAYOUT after, bool discard)
{
	if (cmd.type == D3D12_COMMAND_LIST_TYPE_COPY)
		after = D3D12_BARRIER_LAYOUT_COMMON;
	Transition(cmd, image.resource, discard ? D3D12_BARRIER_LAYOUT_UNDEFINED : image.layout, after, image.layout);
	image.layout = after;
}

} // namespace d3d12

template <>
void Image<kD3D12>::Transition(CommandBufferHandle<kD3D12> cmd, ImageLayout layout, ImageAspect aspectFlags)
{
	if (aspectFlags == ImageAspect::kNone)
		aspectFlags = AspectOf(GetDesc().format);
	d3d12::TransitionImage(*cmd, *std::get<0>(myImage), d3d12::ToD3D12Layout(layout), GetDesc().layout == ImageLayout::kUndefined);
	InternalSetImageLayout(layout);
	InternalSetAspectFlags(aspectFlags);
}

template <>
void Image<kD3D12>::BlitFrom(CommandBufferHandle<kD3D12> /*cmd*/, const Image& /*source*/)
{
	ENSUREF(false, "the D3D12 backend's scaled blits come with its render passes (docs/backends-plan.md, step 3)");
}

template <>
void Image<kD3D12>::GenerateMips(CommandBufferHandle<kD3D12> /*cmd*/, ImageLayout /*layout*/)
{
	ENSUREF(false, "{}: the D3D12 backend generates no mips yet (D3D12 has no blits: a kernel, docs/backends-plan.md, step 4)", GetName());
}

template <>
void Image<kD3D12>::Clear(CommandBufferHandle<kD3D12> cmd, const ClearValue& value, const std::optional<ImageSubresourceRange<kD3D12>>& /*range*/)
{
	ZoneScopedN("Image::Clear");

	// as a render target or depth stencil view of each level (D3D12 clears those, where Vulkan clears a transfer
	// destination), from the image's layout (a transfer destination's, as Vulkan's clear takes it) and back to it. the
	// whole image (the range's fields are Vulkan's)
	auto* object = std::get<0>(myImage);
	auto& allocator = *cmd->device;
	const auto format = GetDesc().format;
	const auto layout = object->layout;
	const bool discard = GetDesc().layout == ImageLayout::kUndefined;
	const bool depth = HasDepthComponent(format);
	ENSUREF(
		(object->flags & (depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) != 0,
		"{}: a clear of an image that can't be rendered to (a block compressed format, or no transfer destination usage)",
		GetName());
	const auto target = depth ? D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE : D3D12_BARRIER_LAYOUT_RENDER_TARGET;
	d3d12::TransitionImage(*cmd, *object, target, discard);
	for (uint32_t level = 0; level < GetDesc().levelCount; level++)
	{
		if (depth)
		{
			const auto view = allocator.depthStencils.Allocate();
			const D3D12_DEPTH_STENCIL_VIEW_DESC desc{
				.Format = d3d12::ToD3D12(format), .ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D, .Flags = D3D12_DSV_FLAG_NONE, .Texture2D = {.MipSlice = level}};
			allocator.device->CreateDepthStencilView(object->resource, &desc, view);
			auto flags = D3D12_CLEAR_FLAG_DEPTH;
			if (HasStencilComponent(format))
				flags |= D3D12_CLEAR_FLAG_STENCIL;
			cmd->list->ClearDepthStencilView(view, flags, value.depth, static_cast<UINT8>(value.stencil), 0, nullptr);
			allocator.depthStencils.Free(view); // a clear reads its view as it's recorded
			continue;
		}
		const auto view = allocator.renderTargets.Allocate();
		D3D12_RENDER_TARGET_VIEW_DESC desc{.Format = d3d12::ToD3D12(format)};
		if (object->dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
		{
			desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
			desc.Texture3D = {.MipSlice = level, .FirstWSlice = 0, .WSize = static_cast<UINT>(-1)};
		}
		else
		{
			desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			desc.Texture2D = {.MipSlice = level, .PlaneSlice = 0};
		}
		allocator.device->CreateRenderTargetView(object->resource, &desc, view);
		cmd->list->ClearRenderTargetView(view, value.color.data(), 0, nullptr);
		allocator.renderTargets.Free(view);
	}
	d3d12::TransitionImage(*cmd, *object, layout);
}

template <>
Image<kD3D12>::Image(CreateDescType&& desc, ValueType&& data)
	: DeviceObject<Image<kD3D12>>(std::forward<CreateDescType>(desc))
	, myImage(std::forward<ValueType>(data))
{}

template <>
Image<kD3D12>::Image(CreateDescType&& desc, const MemoryBlock<kD3D12>& memory, uint64_t offset)
	: Image(
		std::forward<CreateDescType>(desc),
		[&desc, &memory, offset]
		{
			auto& allocator = d3d12::AllocatorObject::ForDevice(desc.device);
			const auto resourceDesc = image::ResourceDesc(desc);
			const auto castable = image::CastableFormats(desc.format);
			auto* object = image::NewObject(desc, resourceDesc);
			// placed where others may alias it: undefined until its first transition (which discards)
			d3d12::Check(
				allocator.allocator->CreateAliasingResource2(
					memory.GetAllocation(),
					offset,
					&resourceDesc,
					D3D12_BARRIER_LAYOUT_UNDEFINED,
					nullptr,
					static_cast<UINT32>(castable.size()),
					castable.empty() ? nullptr : castable.data(),
					IID_PPV_ARGS(&object->resource)),
				std::format("{}: CreateAliasingResource2 (an image at {})", GetDebugName(desc), offset));
			d3d12::SetName(object->resource, desc.name);
			return ValueType{object, nullptr};
		}())
{}

template <>
MemoryRequirements Image<kD3D12>::GetMemoryRequirements(const Device<kD3D12>& device, const CreateDescType& desc)
{
	const auto resourceDesc = image::ResourceDesc(desc);
	const auto info = static_cast<ID3D12Device10*>(device)->GetResourceAllocationInfo2(0, 1, &resourceDesc, nullptr);
	return {.size = info.SizeInBytes, .alignment = info.Alignment, .memoryTypeBits = ~0U};
}

template <>
Image<kD3D12>::Image(CreateDescType&& desc)
	: Image(std::forward<CreateDescType>(desc), [&desc] { return ValueType{image::Create(desc, D3D12_BARRIER_LAYOUT_COMMON), nullptr}; }())
{
	std::get<1>(myImage) = std::get<0>(myImage)->allocation;
}

template <>
Image<kD3D12>::Image(
	CreateDescType&& desc,
	CommandBufferHandle<kD3D12> cmd,
	core::TaskCreateInfo<void>& timlineCallbackOut,
	std::tuple<BufferHandle<kD3D12>, AllocationHandle<kD3D12>>&& initialData,
	uint64_t byteCount)
	: Image(
		std::forward<CreateDescType>(desc),
		[&desc, cmd, &initialData, byteCount]
		{ return ValueType{image::CreateUploaded(*cmd, std::get<0>(initialData), byteCount, desc), nullptr}; }())
{
	std::get<1>(myImage) = std::get<0>(myImage)->allocation;
	InternalSetImageLayout(ImageLayout::kTransferDestination); // as the upload leaves it
	timlineCallbackOut = core::CreateTask(
		[buffer = std::get<0>(initialData), allocation = std::get<1>(initialData)]
		{
			if (allocation != nullptr)
				allocation->Release();
			else
				buffer->Release();
		});
}

template <>
Image<kD3D12>::Image(
	CreateDescType&& desc,
	CommandBufferHandle<kD3D12> cmd,
	const void* initialData,
	size_t initialDataSize,
	core::TaskCreateInfo<void>& timlineCallbackOut)
	: Image(
		std::forward<CreateDescType>(desc),
		cmd,
		timlineCallbackOut,
		// an upload buffer of the data, released once the upload's commands are done
		[&desc, initialData, initialDataSize]
		{
			auto& allocator = d3d12::AllocatorObject::ForDevice(desc.device);
			D3D12MA::ALLOCATION_DESC allocationDesc{};
			allocationDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;
			const D3D12_RESOURCE_DESC1 bufferDesc{
				.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
				.Width = initialDataSize,
				.Height = 1,
				.DepthOrArraySize = 1,
				.MipLevels = 1,
				.SampleDesc = {.Count = 1},
				.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
			D3D12MA::Allocation* allocation = nullptr;
			d3d12::Check(
				allocator.allocator->CreateResource3(
					&allocationDesc, &bufferDesc, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, 0, nullptr, &allocation, IID_NULL, nullptr),
				std::format("{}: CreateResource3 (its upload)", GetDebugName(desc)));
			void* data = nullptr;
			d3d12::Check(allocation->GetResource()->Map(0, nullptr, &data), "ID3D12Resource::Map (an upload)");
			std::memcpy(data, initialData, initialDataSize);
			allocation->GetResource()->Unmap(0, nullptr);
			return std::tuple<BufferHandle<kD3D12>, AllocationHandle<kD3D12>>{allocation->GetResource(), allocation};
		}(),
		initialDataSize)
{}

template <>
Image<kD3D12>::Image(
	CreateDescType&& desc, Buffer<kD3D12>&& staging, CommandBufferHandle<kD3D12> cmd, core::TaskCreateInfo<void>& timlineCallbackOut)
	: Image(
		std::forward<CreateDescType>(desc),
		[&desc, cmd, &staging] { return ValueType{image::CreateUploaded(*cmd, staging.GetBuffer(), staging.GetDesc().size, desc), nullptr}; }())
{
	std::get<1>(myImage) = std::get<0>(myImage)->allocation;
	InternalSetImageLayout(ImageLayout::kTransferDestination); // as the upload leaves it
	timlineCallbackOut = core::CreateTask([staging = std::make_shared<Buffer<kD3D12>>(std::move(staging))] {});
}

template <>
Image<kD3D12>::~Image()
{
	if (!IsValid())
		return;
	d3d12::DeleteImageObject(std::get<0>(myImage));
}

template <>
ImageView<kD3D12>::ImageView(CreateDescType&& desc, ImageViewHandle<kD3D12>&& view)
	: DeviceObject<ImageView<kD3D12>>(std::forward<CreateDescType>(desc))
	, myView(std::forward<ImageViewHandle<kD3D12>>(view))
{}

namespace image
{

static D3D12_SHADER_COMPONENT_MAPPING ToD3D12(ComponentSwizzle swizzle, D3D12_SHADER_COMPONENT_MAPPING identity) noexcept
{
	switch (swizzle)
	{
	case ComponentSwizzle::kIdentity: return identity;
	case ComponentSwizzle::kZero: return D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0;
	case ComponentSwizzle::kOne: return D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1;
	case ComponentSwizzle::kR: return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0;
	case ComponentSwizzle::kG: return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1;
	case ComponentSwizzle::kB: return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2;
	case ComponentSwizzle::kA: return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3;
	}
	return identity;
}

} // namespace image

namespace d3d12
{

ImageViewObject* NewImageView(AllocatorObject& allocator, ImageObject* image, const ImageViewDesc& desc)
{
	const auto levelCount = desc.levelCount == 0 ? image->levelCount - desc.baseLevel : desc.levelCount;
	const bool volume = image->dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
	const bool depthStencil = !HasColorComponent(desc.format);
	auto* view = new d3d12::ImageViewObject{.image = image}; //NOLINT(cppcoreguidelines-owning-memory)

	// a shader's view: sampled, of the levels asked (a depth stencil image's depth or stencil plane as a color format)
	if ((image->flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0)
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC srv{
			.Format = image::ShaderFormat(desc.format, desc.aspectFlags),
			.Shader4ComponentMapping = static_cast<UINT>(D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
				image::ToD3D12(desc.components[0], D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0),
				image::ToD3D12(desc.components[1], D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1),
				image::ToD3D12(desc.components[2], D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2),
				image::ToD3D12(desc.components[3], D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3)))};
		if (volume)
		{
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
			srv.Texture3D = {.MostDetailedMip = desc.baseLevel, .MipLevels = levelCount, .ResourceMinLODClamp = 0.0F};
		}
		else
		{
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srv.Texture2D = {
				.MostDetailedMip = desc.baseLevel,
				.MipLevels = levelCount,
				.PlaneSlice = desc.aspectFlags == ImageAspect::kStencil ? 1U : 0U,
				.ResourceMinLODClamp = 0.0F};
		}
		view->shaderResource = allocator.views.Allocate();
		allocator.device->CreateShaderResourceView(image->resource, &srv, view->shaderResource);
	}
	// a storage image's: its first level (an sRGB format takes none: its unorm kin's, where the code writes it)
	if ((image->flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0)
	{
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav{.Format = d3d12::ToD3D12(desc.format)};
		if (volume)
		{
			uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
			uav.Texture3D = {.MipSlice = desc.baseLevel, .FirstWSlice = 0, .WSize = static_cast<UINT>(-1)};
		}
		else
		{
			uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			uav.Texture2D = {.MipSlice = desc.baseLevel, .PlaneSlice = 0};
		}
		view->unorderedAccess = allocator.views.Allocate();
		allocator.device->CreateUnorderedAccessView(image->resource, nullptr, &uav, view->unorderedAccess);
	}
	// an attachment's: its first level
	if ((image->flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0 && !depthStencil)
	{
		D3D12_RENDER_TARGET_VIEW_DESC rtv{.Format = d3d12::ToD3D12(desc.format)};
		if (volume)
		{
			rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
			rtv.Texture3D = {.MipSlice = desc.baseLevel, .FirstWSlice = 0, .WSize = static_cast<UINT>(-1)};
		}
		else
		{
			rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			rtv.Texture2D = {.MipSlice = desc.baseLevel, .PlaneSlice = 0};
		}
		view->renderTarget = allocator.renderTargets.Allocate();
		allocator.device->CreateRenderTargetView(image->resource, &rtv, view->renderTarget);
	}
	if ((image->flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0)
	{
		const D3D12_DEPTH_STENCIL_VIEW_DESC dsv{
			.Format = d3d12::ToD3D12(image->format),
			.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D,
			.Flags = D3D12_DSV_FLAG_NONE,
			.Texture2D = {.MipSlice = desc.baseLevel}};
		view->depthStencil = allocator.depthStencils.Allocate();
		allocator.device->CreateDepthStencilView(image->resource, &dsv, view->depthStencil);
	}
	return view;
}

void DeleteImageView(AllocatorObject& allocator, ImageViewObject* view) noexcept
{
	if (view == nullptr)
		return;
	allocator.views.Free(view->shaderResource);
	allocator.views.Free(view->unorderedAccess);
	allocator.renderTargets.Free(view->renderTarget);
	allocator.depthStencils.Free(view->depthStencil);
	delete view; //NOLINT(cppcoreguidelines-owning-memory)
}

} // namespace d3d12

template <>
ImageView<kD3D12>::ImageView(CreateDescType&& desc)
	: ImageView(
		std::forward<CreateDescType>(desc),
		[&desc]
		{
			auto* image = desc.image.handle;
			ENSUREF(image != nullptr, "{}: a view of no image", GetDebugName(desc));
			return d3d12::NewImageView(
				d3d12::AllocatorObject::ForDevice(desc.device),
				image,
				{.format = desc.format,
				 .aspectFlags = desc.aspectFlags,
				 .levelCount = desc.levelCount,
				 .components = desc.components,
				 .baseLevel = desc.baseLevel});
		}())
{}

template <>
ImageView<kD3D12>::~ImageView()
{
	if (!IsValid() || myView == nullptr)
		return;
	d3d12::DeleteImageView(d3d12::AllocatorObject::ForDevice(GetDesc().device), myView);
}

} // namespace rhi
