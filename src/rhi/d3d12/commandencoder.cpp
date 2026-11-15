// the D3D12 backend's commands: dispatches with what the pipeline bound for them (CommandBufferObject::PrepareDispatch),
// enhanced barriers (synchronization2's model: global barriers between commands, an image's layouts in its own, see
// image.cpp), and copies between buffers and images on any list. draws come with the render passes (step 3). a copy
// between a buffer and an image takes the buffer's rows packed, as rhi's other backends do, which D3D12 allows where the
// device copies with unrestricted pitches (D3D12_FEATURE_DATA_D3D12_OPTIONS13; else rows 256 bytes apart: fails hard)
#include <rhi/d3d12/backend.h>

#include <rhi/accelerationstructure.h>
#include <rhi/buffer.h>
#include <rhi/command.h>
#include <rhi/commandencoder.h>
#include <rhi/image.h>
#include <rhi/querypool.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace rhi
{

namespace commandencoder
{

// a buffer of the host's data in the upload heap, the command buffer's until its commands are done
static ID3D12Resource* Upload(d3d12::CommandBufferObject& cmd, const void* data, uint64_t size)
{
	D3D12MA::ALLOCATION_DESC allocationDesc{};
	allocationDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;
	const D3D12_RESOURCE_DESC1 desc{
		.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
		.Width = size,
		.Height = 1,
		.DepthOrArraySize = 1,
		.MipLevels = 1,
		.SampleDesc = {.Count = 1},
		.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
	D3D12MA::Allocation* allocation = nullptr;
	d3d12::Check(
		cmd.device->allocator->CreateResource3(&allocationDesc, &desc, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, 0, nullptr, &allocation, IID_NULL, nullptr),
		"CreateResource3 (an upload)");
	void* mapped = nullptr;
	d3d12::Check(allocation->GetResource()->Map(0, nullptr, &mapped), "ID3D12Resource::Map (an upload)");
	std::memcpy(mapped, data, size);
	allocation->GetResource()->Unmap(0, nullptr);
	cmd.transients.push_back(allocation);
	return allocation->GetResource();
}

// a buffer's part as a texture copy's footprint: rows of blocks packed (see the file's comment)
static D3D12_TEXTURE_COPY_LOCATION BufferFootprint(ID3D12Resource* buffer, uint64_t offset, DXGI_FORMAT format, Format rhiFormat, Extent3d extent)
{
	const auto block = BlockOf(rhiFormat);
	const auto rowPitch = static_cast<UINT>(uint64_t{(extent.width + block.width - 1) / block.width} * block.byteCount);
	return D3D12_TEXTURE_COPY_LOCATION{
		.pResource = buffer,
		.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
		.PlacedFootprint = {
			.Offset = offset,
			.Footprint = {.Format = format, .Width = extent.width, .Height = extent.height, .Depth = std::max(extent.depth, 1U), .RowPitch = rowPitch}}};
}

// a barrier's side as the list's queue type takes it: a compute list's shading is compute's, and it neither draws nor
// resolves; a copy list copies alone. what's left of no work takes no access (D3D12's 0 is COMMON, any: none is
// NO_ACCESS)
static std::pair<D3D12_BARRIER_SYNC, D3D12_BARRIER_ACCESS> ForList(D3D12_BARRIER_SYNC sync, D3D12_BARRIER_ACCESS access, D3D12_COMMAND_LIST_TYPE type)
{
	if (type == D3D12_COMMAND_LIST_TYPE_COMPUTE)
	{
		constexpr auto kGraphics = D3D12_BARRIER_SYNC_DRAW | D3D12_BARRIER_SYNC_INDEX_INPUT | D3D12_BARRIER_SYNC_VERTEX_SHADING |
								   D3D12_BARRIER_SYNC_PIXEL_SHADING | D3D12_BARRIER_SYNC_DEPTH_STENCIL | D3D12_BARRIER_SYNC_RENDER_TARGET |
								   D3D12_BARRIER_SYNC_ALL_SHADING | D3D12_BARRIER_SYNC_NON_PIXEL_SHADING;
		if ((sync & kGraphics) != D3D12_BARRIER_SYNC_NONE)
			sync = (sync & ~kGraphics) | D3D12_BARRIER_SYNC_COMPUTE_SHADING;
		sync &= ~D3D12_BARRIER_SYNC_RESOLVE;
		access &= ~(D3D12_BARRIER_ACCESS_RENDER_TARGET | D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ | D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE |
					D3D12_BARRIER_ACCESS_INDEX_BUFFER | D3D12_BARRIER_ACCESS_VERTEX_BUFFER | D3D12_BARRIER_ACCESS_RESOLVE_SOURCE |
					D3D12_BARRIER_ACCESS_RESOLVE_DEST);
	}
	else if (type == D3D12_COMMAND_LIST_TYPE_COPY)
	{
		sync = sync != D3D12_BARRIER_SYNC_NONE ? D3D12_BARRIER_SYNC_COPY : sync;
		access &= D3D12_BARRIER_ACCESS_COPY_SOURCE | D3D12_BARRIER_ACCESS_COPY_DEST | D3D12_BARRIER_ACCESS_NO_ACCESS;
	}
	if (sync == D3D12_BARRIER_SYNC_NONE || access == D3D12_BARRIER_ACCESS_COMMON)
		access = D3D12_BARRIER_ACCESS_NO_ACCESS;
	// each sync with an access it's compatible with and each access with a sync, as D3D12 wants (the debug layer's error
	// 1331), where rhi's barriers join resources' (the frame graph's: a copy's write of one buffer and a compute stage's
	// reads of another, compute's sync and a copy's access): the side made whole by what each takes, which waits and
	// flushes more, never less. what an attachment's layout is (a render target's, depth's) is left to its texture barriers
	if (access != D3D12_BARRIER_ACCESS_NO_ACCESS && sync != D3D12_BARRIER_SYNC_ALL)
	{
		constexpr auto kShading = D3D12_BARRIER_SYNC_VERTEX_SHADING | D3D12_BARRIER_SYNC_PIXEL_SHADING |
								  D3D12_BARRIER_SYNC_COMPUTE_SHADING | D3D12_BARRIER_SYNC_ALL_SHADING |
								  D3D12_BARRIER_SYNC_NON_PIXEL_SHADING | D3D12_BARRIER_SYNC_DRAW;
		constexpr auto kShaderAccess =
			D3D12_BARRIER_ACCESS_UNORDERED_ACCESS | D3D12_BARRIER_ACCESS_SHADER_RESOURCE | D3D12_BARRIER_ACCESS_CONSTANT_BUFFER;
		constexpr auto kCopyAccess = D3D12_BARRIER_ACCESS_COPY_SOURCE | D3D12_BARRIER_ACCESS_COPY_DEST;
		auto has = [](auto flags, auto bits) { return (flags & bits) != 0; };
		// an attachment's syncs without its accesses (a global barrier's, which can't take them): the draws' whole
		constexpr auto kAttachmentSync = D3D12_BARRIER_SYNC_RENDER_TARGET | D3D12_BARRIER_SYNC_DEPTH_STENCIL;
		constexpr auto kAttachmentAccess = D3D12_BARRIER_ACCESS_RENDER_TARGET | D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ |
										   D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE;
		if (has(sync, kAttachmentSync) && !has(access, kAttachmentAccess))
			sync = (sync & ~kAttachmentSync) | D3D12_BARRIER_SYNC_DRAW;
		// a UAV clear's sync (rhi's transfer stage's, FillBuffer's) with its unordered access
		if (has(sync, D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW) && !has(access, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS))
			access |= D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
		// the acceleration structures' syncs: a build's, a copy's or a postbuild info's (rhi's build stage, all three)
		// with their structures' reads where none of theirs is there, and their accesses with a build's sync
		constexpr auto kStructureSync = D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE |
										D3D12_BARRIER_SYNC_COPY_RAYTRACING_ACCELERATION_STRUCTURE |
										D3D12_BARRIER_SYNC_EMIT_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO;
		constexpr auto kStructureAccess =
			D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ | D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE;
		// (each its own: a postbuild info's reads a structure and writes a buffer, never a structure)
		if (has(sync, D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE | D3D12_BARRIER_SYNC_COPY_RAYTRACING_ACCELERATION_STRUCTURE) &&
			!has(access, kStructureAccess))
			access |= D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ;
		if (has(sync, D3D12_BARRIER_SYNC_EMIT_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO) &&
			!has(access, D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ | D3D12_BARRIER_ACCESS_UNORDERED_ACCESS))
			access |= D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ;
		if (has(access, kStructureAccess) && !has(sync, kStructureSync | kShading))
			sync |= D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE;
		// the syncs' accesses
		if (has(sync, kShading) && !has(access, kShaderAccess))
			access |= D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
		if (has(sync, D3D12_BARRIER_SYNC_COPY) && !has(access, kCopyAccess))
			access |= D3D12_BARRIER_ACCESS_COPY_DEST;
		if (has(sync, D3D12_BARRIER_SYNC_EXECUTE_INDIRECT) && !has(access, D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT))
			access |= D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT;
		// the accesses' syncs
		if (has(access, kShaderAccess) && !has(sync, kShading | D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW))
			sync |= type == D3D12_COMMAND_LIST_TYPE_COMPUTE ? D3D12_BARRIER_SYNC_COMPUTE_SHADING : D3D12_BARRIER_SYNC_ALL_SHADING;
		if (has(access, kCopyAccess) && !has(sync, D3D12_BARRIER_SYNC_COPY))
			sync |= D3D12_BARRIER_SYNC_COPY;
		if (has(access, D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT) && !has(sync, D3D12_BARRIER_SYNC_EXECUTE_INDIRECT))
			sync |= D3D12_BARRIER_SYNC_EXECUTE_INDIRECT;
	}
	return {sync, access};
}

static void EnsureUnrestrictedCopies(ID3D12Device* device)
{
	static const bool unrestricted = [device]
	{
		D3D12_FEATURE_DATA_D3D12_OPTIONS13 options{};
		return SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS13, &options, sizeof(options))) &&
			   options.UnrestrictedBufferTextureCopyPitchSupported;
	}();
	ENSUREF(unrestricted, "the device copies between buffers and images only with rows 256 bytes apart (no unrestricted pitches): update the driver");
}

} // namespace commandencoder

template <>
void CommandEncoder<kD3D12>::SetViewport(const Viewport& viewport) const
{
	const D3D12_VIEWPORT d3dViewport{
		.TopLeftX = viewport.x, .TopLeftY = viewport.y, .Width = viewport.width, .Height = viewport.height, .MinDepth = viewport.minDepth, .MaxDepth = viewport.maxDepth};
	myCmd->list->RSSetViewports(1, &d3dViewport);
}

template <>
void CommandEncoder<kD3D12>::SetScissor(const Rect& rect) const
{
	const D3D12_RECT d3dRect{
		.left = rect.x, .top = rect.y, .right = rect.x + static_cast<LONG>(rect.width), .bottom = rect.y + static_cast<LONG>(rect.height)};
	myCmd->list->RSSetScissorRects(1, &d3dRect);
}

// the cull mode and front face are a graphics pipeline state's in D3D12: the next draw's state is the bound pipeline's
// for them (PipelineObject::State)
template <>
void CommandEncoder<kD3D12>::SetCullMode(CullMode mode) const
{
	myCmd->cullMode = mode;
}

template <>
void CommandEncoder<kD3D12>::SetFrontFace(FrontFace face) const
{
	myCmd->frontFace = face;
}

template <>
void CommandEncoder<kD3D12>::SetStencilReference(uint32_t reference) const
{
	myCmd->list->OMSetStencilRef(reference);
}

template <>
void CommandEncoder<kD3D12>::BindIndexBuffer(const Buffer<kD3D12>& buffer, uint64_t offset, IndexType type) const
{
	ID3D12Resource* resource = buffer;
	const D3D12_INDEX_BUFFER_VIEW view{
		.BufferLocation = resource->GetGPUVirtualAddress() + offset,
		.SizeInBytes = static_cast<UINT>(resource->GetDesc().Width - offset),
		.Format = type == IndexType::kUint32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT};
	myCmd->list->IASetIndexBuffer(&view);
}

template <>
void CommandEncoder<kD3D12>::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) const
{
	ZoneScopedN("CommandEncoder::Draw");

	myCmd->PrepareDraw();
	myCmd->list->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
}

template <>
void CommandEncoder<kD3D12>::DrawIndexed(
	uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) const
{
	ZoneScopedN("CommandEncoder::DrawIndexed");

	myCmd->PrepareDraw();
	myCmd->list->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

// rhi's indexed commands are D3D12's draw arguments (index count, instance count, first index, vertex offset, first
// instance, which the vertex stage takes as SV_StartInstanceLocation), executed by a signature of their stride
template <>
void CommandEncoder<kD3D12>::DrawIndexedIndirect(const Buffer<kD3D12>& buffer, uint64_t offset, uint32_t drawCount, uint32_t stride) const
{
	ZoneScopedN("CommandEncoder::DrawIndexedIndirect");

	myCmd->PrepareDraw();
	myCmd->list->ExecuteIndirect(myCmd->device->DrawIndexedSignature(stride), drawCount, buffer, offset, nullptr, 0);
}

template <>
void CommandEncoder<kD3D12>::DrawIndexedIndirectCount(
	const Buffer<kD3D12>& buffer, uint64_t offset, const Buffer<kD3D12>& countBuffer, uint64_t countOffset, uint32_t maxDrawCount, uint32_t stride) const
{
	ZoneScopedN("CommandEncoder::DrawIndexedIndirectCount");

	myCmd->PrepareDraw();
	myCmd->list->ExecuteIndirect(myCmd->device->DrawIndexedSignature(stride), maxDrawCount, buffer, offset, countBuffer, countOffset);
}

template <>
void CommandEncoder<kD3D12>::CopyImageToBuffer(ImageHandle<kD3D12> image, Extent2d extent, const Buffer<kD3D12>& buffer) const
{
	ZoneScopedN("CommandEncoder::CopyImageToBuffer");

	commandencoder::EnsureUnrestrictedCopies(myCmd->device->device);
	const auto destination = commandencoder::BufferFootprint(
		buffer, 0, image->resource->GetDesc().Format, image->format, Extent3d{.width = extent.width, .height = extent.height, .depth = 1});
	const D3D12_TEXTURE_COPY_LOCATION source{.pResource = image->resource, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0};
	const D3D12_BOX box{.left = 0, .top = 0, .front = 0, .right = extent.width, .bottom = extent.height, .back = 1};
	myCmd->list->CopyTextureRegion(&destination, 0, 0, 0, &source, &box);
}

template <>
void CommandEncoder<kD3D12>::CopyImageToBuffer(const Image<kD3D12>& image, const Buffer<kD3D12>& buffer) const
{
	ZoneScopedN("CommandEncoder::CopyImageToBuffer");

	ImageHandle<kD3D12> object = image;
	commandencoder::EnsureUnrestrictedCopies(myCmd->device->device);
	const auto destination =
		commandencoder::BufferFootprint(buffer, 0, object->resource->GetDesc().Format, image.GetDesc().format, image.GetDesc().extent);
	const D3D12_TEXTURE_COPY_LOCATION source{.pResource = object->resource, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0};
	myCmd->list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
}

template <>
void CommandEncoder<kD3D12>::CopyBufferToImage(const Buffer<kD3D12>& buffer, const Image<kD3D12>& image, const Rect& region) const
{
	ZoneScopedN("CommandEncoder::CopyBufferToImage");

	ImageHandle<kD3D12> object = image;
	commandencoder::EnsureUnrestrictedCopies(myCmd->device->device);
	const auto source = commandencoder::BufferFootprint(
		buffer, 0, object->resource->GetDesc().Format, image.GetDesc().format, Extent3d{.width = region.width, .height = region.height, .depth = 1});
	const D3D12_TEXTURE_COPY_LOCATION destination{.pResource = object->resource, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0};
	myCmd->list->CopyTextureRegion(&destination, static_cast<UINT>(region.x), static_cast<UINT>(region.y), 0, &source, nullptr);
}

template <>
void CommandEncoder<kD3D12>::UpdateBuffer(const Buffer<kD3D12>& buffer, uint64_t offset, std::span<const std::byte> data) const
{
	ZoneScopedN("CommandEncoder::UpdateBuffer");

	// the data in an upload buffer of its own (the command buffer's until its commands are done), copied in order with the
	// other commands
	constexpr size_t kMaxUpdateByteCount = 65536;
	ENSUREF(data.size() <= kMaxUpdateByteCount && data.size() % 4 == 0, "UpdateBuffer takes at most 64 kB, a multiple of 4 bytes ({})", data.size());
	auto* source = commandencoder::Upload(*myCmd, data.data(), data.size());
	myCmd->list->CopyBufferRegion(buffer, offset, source, 0, data.size());
}

template <>
void CommandEncoder<kD3D12>::ExecuteCommands(std::vector<CommandBuffer<kD3D12>>&& secondaries) const
{
	// their commands were recorded inline, into this command buffer's list (see CommandPool::BeginSecondary): each is
	// ended and handed back to its pool, and what they set on the list is the primary's to set again (as after Vulkan's
	// vkCmdExecuteCommands, its state is undefined): its root signatures, which its next dispatch or draw sets
	for (auto& secondary : secondaries)
	{
		ENSURE(secondary.IsSecondary());
		ENSUREF(static_cast<CommandBufferHandle<kD3D12>>(secondary)->primary == myCmd, "a secondary command buffer executed by another than its primary");
		if (secondary.IsRecording())
			secondary.End();
		(void)secondary.Release();
	}
	myCmd->computeRootSignature = nullptr;
	myCmd->graphicsRootSignature = nullptr;
}

template <>
void CommandEncoder<kD3D12>::FillBuffer(const Buffer<kD3D12>& buffer, uint64_t offset, uint64_t size, uint32_t value) const
{
	ZoneScopedN("CommandEncoder::FillBuffer");

	ID3D12Resource* target = buffer;
	const auto desc = target->GetDesc();
	const uint64_t byteCount = size != 0 ? size : desc.Width - offset;
	ENSUREF(byteCount % 4 == 0 && offset % 4 == 0, "FillBuffer fills a multiple of 4 bytes, from a multiple of 4 ({} at {})", byteCount, offset);
	// a clear of a raw view of it (the gpu's: the GI's buffers, megabytes, cleared from uploads took 6 ms, and the OIT's
	// and the upscaler's 0.25 ms a frame each), where it takes unordered access and the list clears (not a copy list's):
	// its view in a range of the shader visible heap the command buffer takes, and in the cpu heap, which the clear reads
	// as it's recorded. rhi's transfer stage is D3D12's copy and UAV clear both (ToD3D12Sync)
	if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0 && myCmd->type != D3D12_COMMAND_LIST_TYPE_COPY)
	{
		auto& allocator = *myCmd->device;
		const D3D12_UNORDERED_ACCESS_VIEW_DESC view{
			.Format = DXGI_FORMAT_R32_TYPELESS,
			.ViewDimension = D3D12_UAV_DIMENSION_BUFFER,
			.Buffer = {
				.FirstElement = offset / 4,
				.NumElements = static_cast<UINT>(byteCount / 4),
				.StructureByteStride = 0,
				.CounterOffsetInBytes = 0,
				.Flags = D3D12_BUFFER_UAV_FLAG_RAW}};
		const uint32_t shaderVisible = allocator.pool.resources.Allocate(1);
		myCmd->pushedResources.emplace_back(shaderVisible, 1);
		const auto cpu = allocator.views.Allocate();
		allocator.device->CreateUnorderedAccessView(target, nullptr, &view, cpu);
		allocator.device->CreateUnorderedAccessView(target, nullptr, &view, allocator.pool.resources.Cpu(shaderVisible));
		const std::array<UINT, 4> values{value, value, value, value};
		myCmd->list->ClearUnorderedAccessViewUint(allocator.pool.resources.Gpu(shaderVisible), cpu, target, values.data(), 0, nullptr);
		allocator.views.Free(cpu);
		return;
	}
	// else an upload buffer of the value, copied in (the command buffer's until its commands are done, see UpdateBuffer)
	std::vector<uint32_t> values(byteCount / 4, value);
	auto* source = commandencoder::Upload(*myCmd, values.data(), byteCount);
	myCmd->list->CopyBufferRegion(target, offset, source, 0, byteCount);
}

template <>
void CommandEncoder<kD3D12>::DispatchIndirect(const Buffer<kD3D12>& buffer, uint64_t offset) const
{
	ZoneScopedN("CommandEncoder::DispatchIndirect");

	myCmd->PrepareDispatch();
	myCmd->list->ExecuteIndirect(myCmd->device->dispatchSignature, 1, buffer, offset, nullptr, 0);
}

// queue family ownership: D3D12 has none (a buffer is any queue's; an image's layout says which queues may use it, see
// image.cpp's transitions)
template <>
void CommandEncoder<kD3D12>::ReleaseOwnership(const Buffer<kD3D12>&, uint32_t, uint32_t, PipelineStage, Access) const
{}

template <>
void CommandEncoder<kD3D12>::AcquireOwnership(const Buffer<kD3D12>&, uint32_t, uint32_t, PipelineStage, Access) const
{}

template <>
void CommandEncoder<kD3D12>::ReleaseOwnership(const Image<kD3D12>&, uint32_t, uint32_t, PipelineStage, Access) const
{}

template <>
void CommandEncoder<kD3D12>::AcquireOwnership(const Image<kD3D12>&, uint32_t, uint32_t, PipelineStage, Access) const
{}

template <>
void CommandEncoder<kD3D12>::WriteAccelerationStructureCompactedSize(
	const AccelerationStructure<kD3D12>& structure, const QueryPool<kD3D12>& pool, uint32_t query) const
{
	// its postbuild info written to the pool's buffer (unordered access) and copied into its readback buffer, which the
	// host reads (QueryPool::Read); the buffer back to unordered access for the next
	QueryPoolHandle<kD3D12> object = pool;
	ENSUREF(object->postbuild != nullptr, "a compacted size written to a query pool of timestamps");
	ENSURE(query < pool.GetDesc().count);
	auto* postbuild = object->postbuild->GetResource();
	const uint64_t offset = uint64_t{query} * sizeof(uint64_t);
	const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC desc{
		.DestBuffer = postbuild->GetGPUVirtualAddress() + offset,
		.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE};
	const D3D12_GPU_VIRTUAL_ADDRESS source = structure.GetDeviceAddress();
	myCmd->list->EmitRaytracingAccelerationStructurePostbuildInfo(&desc, 1, &source);
	auto barrier = [this, postbuild](D3D12_BARRIER_SYNC syncBefore, D3D12_BARRIER_ACCESS accessBefore, D3D12_BARRIER_SYNC syncAfter, D3D12_BARRIER_ACCESS accessAfter)
	{
		const D3D12_BUFFER_BARRIER buffer{
			.SyncBefore = syncBefore,
			.SyncAfter = syncAfter,
			.AccessBefore = accessBefore,
			.AccessAfter = accessAfter,
			.pResource = postbuild,
			.Offset = 0,
			.Size = UINT64_MAX};
		const D3D12_BARRIER_GROUP group{.Type = D3D12_BARRIER_TYPE_BUFFER, .NumBarriers = 1, .pBufferBarriers = &buffer};
		myCmd->list->Barrier(1, &group);
	};
	barrier(
		D3D12_BARRIER_SYNC_EMIT_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO,
		D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
		D3D12_BARRIER_SYNC_COPY,
		D3D12_BARRIER_ACCESS_COPY_SOURCE);
	myCmd->list->CopyBufferRegion(object->readback->GetResource(), offset, postbuild, offset, sizeof(uint64_t));
	barrier(
		D3D12_BARRIER_SYNC_COPY,
		D3D12_BARRIER_ACCESS_COPY_SOURCE,
		D3D12_BARRIER_SYNC_EMIT_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO,
		D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
}

template <>
void CommandEncoder<kD3D12>::CopyBuffer(const Buffer<kD3D12>& source, const Buffer<kD3D12>& destination, uint64_t size, uint64_t offset) const
{
	ZoneScopedN("CommandEncoder::CopyBuffer");

	myCmd->list->CopyBufferRegion(destination, offset, source, offset, size);
}

template <>
void CommandEncoder<kD3D12>::DispatchThreads(const ComputeLaunchParameters& launch, uint32_t threadsX, uint32_t threadsY, uint32_t threadsZ) const
{
	ZoneScopedN("CommandEncoder::DispatchThreads");

	auto groups = [](uint32_t threads, uint64_t size) { return static_cast<UINT>((threads + size - 1) / size); };
	myCmd->PrepareDispatch();
	myCmd->list->Dispatch(
		groups(threadsX, launch.threadGroupSize[0]), groups(threadsY, launch.threadGroupSize[1]), groups(threadsZ, launch.threadGroupSize[2]));
}

template <>
void CommandEncoder<kD3D12>::Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) const
{
	ZoneScopedN("CommandEncoder::Dispatch");

	myCmd->PrepareDispatch();
	myCmd->list->Dispatch(groupCountX, groupCountY, groupCountZ);
}

template <>
void CommandEncoder<kD3D12>::Barrier(PipelineStage srcStages, Access srcAccess, PipelineStage dstStages, Access dstAccess) const
{
	// a global barrier (every buffer's and image's memory; an image's layout changes are its own, see Image::Transition).
	// a side without work to wait for or before (none: the host's, the top of the pipe) takes no access
	// the attachments' accesses are their layouts' in D3D12, which a global barrier doesn't change (the debug layer's error
	// 1331): left out, the stages kept (a render target's writes are ordered within a queue)
	constexpr auto kLayoutAccess = D3D12_BARRIER_ACCESS_RENDER_TARGET | D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ |
								   D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE | D3D12_BARRIER_ACCESS_RESOLVE_SOURCE |
								   D3D12_BARRIER_ACCESS_RESOLVE_DEST;
	auto global = [](D3D12_BARRIER_ACCESS access)
	{
		if (access == D3D12_BARRIER_ACCESS_COMMON || access == D3D12_BARRIER_ACCESS_NO_ACCESS)
			return access;
		const auto rest = static_cast<D3D12_BARRIER_ACCESS>(access & ~kLayoutAccess);
		return rest != D3D12_BARRIER_ACCESS_COMMON ? rest : D3D12_BARRIER_ACCESS_NO_ACCESS;
	};
	auto [syncBefore, accessBefore] = commandencoder::ForList(d3d12::ToD3D12Sync(srcStages), global(d3d12::ToD3D12Access(srcAccess)), myCmd->type);
	auto [syncAfter, accessAfter] = commandencoder::ForList(d3d12::ToD3D12Sync(dstStages), global(d3d12::ToD3D12Access(dstAccess)), myCmd->type);
	if (syncBefore == D3D12_BARRIER_SYNC_NONE && syncAfter == D3D12_BARRIER_SYNC_NONE)
		return;
	D3D12_GLOBAL_BARRIER barrier{.SyncBefore = syncBefore, .SyncAfter = syncAfter, .AccessBefore = accessBefore, .AccessAfter = accessAfter};
	const D3D12_BARRIER_GROUP group{.Type = D3D12_BARRIER_TYPE_GLOBAL, .NumBarriers = 1, .pGlobalBarriers = &barrier};
	myCmd->list->Barrier(1, &group);
}

} // namespace rhi
