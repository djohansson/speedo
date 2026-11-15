// the D3D12 backend's shared parts (backend.h): results checked, names, the descriptor heaps, the device's state found
// from its handle, and rhi's vocabulary as D3D12's
#include <rhi/d3d12/backend.h>

#include <core/assert.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <string_view>

namespace rhi::d3d12
{

void Check(HRESULT result, std::string_view what)
{
	ENSUREF(SUCCEEDED(result), "D3D12: {} failed ({:#010x})", what, static_cast<uint32_t>(result));
}

void SetName(ID3D12Object* object, std::string_view name)
{
	if (object == nullptr || name.empty())
		return;
	const int count = MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0);
	std::wstring wide(static_cast<size_t>(count), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), wide.data(), count);
	object->SetName(wide.c_str());
}

void CpuDescriptorHeap::Init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
	myDevice = device;
	myType = type;
	myIncrement = device->GetDescriptorHandleIncrementSize(type);
}

void CpuDescriptorHeap::Destroy() noexcept
{
	for (auto*& page : myPages)
		Release(page);
	myPages.clear();
	myFree.clear();
}

D3D12_CPU_DESCRIPTOR_HANDLE CpuDescriptorHeap::Allocate()
{
	std::scoped_lock lock{myMutex};
	if (myFree.empty())
	{
		const D3D12_DESCRIPTOR_HEAP_DESC desc{.Type = myType, .NumDescriptors = kPageSize, .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE, .NodeMask = 0};
		ID3D12DescriptorHeap* page = nullptr;
		Check(myDevice->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&page)), "CreateDescriptorHeap (a cpu page)");
		myPages.push_back(page);
		const auto start = page->GetCPUDescriptorHandleForHeapStart();
		for (uint32_t descriptorIt = kPageSize; descriptorIt-- > 0;)
			myFree.push_back({.ptr = start.ptr + (size_t{descriptorIt} * myIncrement)});
	}
	auto handle = myFree.back();
	myFree.pop_back();
	return handle;
}

void CpuDescriptorHeap::Free(D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept
{
	if (handle.ptr == 0)
		return;
	std::scoped_lock lock{myMutex};
	myFree.push_back(handle);
}

void GpuDescriptorHeap::Init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t capacity, const char* name)
{
	myType = type;
	myName = name;
	myCapacity = capacity;
	myIncrement = device->GetDescriptorHandleIncrementSize(type);
	const D3D12_DESCRIPTOR_HEAP_DESC desc{.Type = type, .NumDescriptors = capacity, .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, .NodeMask = 0};
	Check(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&myHeap)), std::format("CreateDescriptorHeap ({}, {} descriptors)", name, capacity));
	SetName(myHeap, name);
	myCpuStart = myHeap->GetCPUDescriptorHandleForHeapStart();
	myGpuStart = myHeap->GetGPUDescriptorHandleForHeapStart();
	myFree = {{.offset = 0, .count = capacity}};
}

void GpuDescriptorHeap::Destroy() noexcept
{
	Release(myHeap);
	myFree.clear();
}

uint32_t GpuDescriptorHeap::Allocate(uint32_t count)
{
	if (count == 0)
		return kNone;
	std::scoped_lock lock{myMutex};
	auto rangeIt = std::ranges::find_if(myFree, [count](const Range& range) { return range.count >= count; });
	ENSUREF(
		rangeIt != myFree.end(),
		"the shader visible heap of {} ({} descriptors) has no {} free in a row: a capacity of D3D12's",
		myName,
		myCapacity,
		count);
	const uint32_t offset = rangeIt->offset;
	rangeIt->offset += count;
	rangeIt->count -= count;
	if (rangeIt->count == 0)
		myFree.erase(rangeIt);
	return offset;
}

void GpuDescriptorHeap::Free(uint32_t offset, uint32_t count) noexcept
{
	if (offset == kNone || count == 0)
		return;
	std::scoped_lock lock{myMutex};
	auto nextIt = std::ranges::upper_bound(myFree, offset, {}, &Range::offset);
	nextIt = myFree.insert(nextIt, {.offset = offset, .count = count});
	// merged with the next range, then the previous one
	if (auto after = std::next(nextIt); after != myFree.end() && nextIt->offset + nextIt->count == after->offset)
	{
		nextIt->count += after->count;
		myFree.erase(after);
	}
	if (nextIt != myFree.begin())
		if (auto before = std::prev(nextIt); before->offset + before->count == nextIt->offset)
		{
			before->count += nextIt->count;
			myFree.erase(nextIt);
		}
}

namespace
{

// the device's private data naming its AllocatorObject ({5d3a9c62-1f0e-4b8a-9b6e-7c2d4e81a0f3})
constexpr GUID kAllocatorObjectGuid{0x5d3a9c62, 0x1f0e, 0x4b8a, {0x9b, 0x6e, 0x7c, 0x2d, 0x4e, 0x81, 0xa0, 0xf3}};

} // namespace

AllocatorObject& AllocatorObject::ForDevice(ID3D12Device* device)
{
	AllocatorObject* object = nullptr;
	UINT size = sizeof(object);
	Check(device->GetPrivateData(kAllocatorObjectGuid, &size, static_cast<void*>(&object)), "the device's allocator (GetPrivateData)");
	ENSURE(object != nullptr);
	return *object;
}

void RegisterAllocator(ID3D12Device* device, AllocatorObject* allocator)
{
	Check(device->SetPrivateData(kAllocatorObjectGuid, sizeof(allocator), static_cast<const void*>(&allocator)), "SetPrivateData");
}

const DescriptorSetLayoutObject::Binding& DescriptorSetLayoutObject::Find(uint32_t binding) const
{
	auto bindingIt = std::ranges::lower_bound(bindings, binding, {}, &Binding::binding);
	ENSUREF(bindingIt != bindings.end() && bindingIt->binding == binding, "the descriptor set layout has no binding {}", binding);
	return *bindingIt;
}

DXGI_FORMAT ToD3D12(Format format)
{
	switch (format)
	{
	case Format::kUndefined: return DXGI_FORMAT_UNKNOWN;
	case Format::kR8G8B8A8Unorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
	case Format::kR8G8B8A8Srgb: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	case Format::kB8G8R8A8Unorm: return DXGI_FORMAT_B8G8R8A8_UNORM;
	case Format::kB8G8R8A8Srgb: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	// Vulkan names a packed format's components from its top bits, DXGI from its bottom ones: A2B10G10R10 is DXGI's
	// R10G10B10A2 (red in the low bits); A2R10G10B10 (blue in them) has no DXGI format
	case Format::kA2B10G10R10UnormPack32: return DXGI_FORMAT_R10G10B10A2_UNORM;
	case Format::kR16G16B16A16Sfloat: return DXGI_FORMAT_R16G16B16A16_FLOAT;
	case Format::kR16Sfloat: return DXGI_FORMAT_R16_FLOAT;
	case Format::kD32Sfloat: return DXGI_FORMAT_D32_FLOAT;
	case Format::kD32SfloatS8Uint: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
	case Format::kD24UnormS8Uint: return DXGI_FORMAT_D24_UNORM_S8_UINT;
	// DXGI's BC1 has the punch through alpha Vulkan's RGB variant ignores (a block using it decodes alpha 0, not 1), as
	// Metal's has
	case Format::kBC1RgbUnorm: return DXGI_FORMAT_BC1_UNORM;
	case Format::kBC1RgbSrgb: return DXGI_FORMAT_BC1_UNORM_SRGB;
	case Format::kBC3Unorm: return DXGI_FORMAT_BC3_UNORM;
	case Format::kBC3Srgb: return DXGI_FORMAT_BC3_UNORM_SRGB;
	case Format::kBC4Unorm: return DXGI_FORMAT_BC4_UNORM;
	case Format::kBC5Unorm: return DXGI_FORMAT_BC5_UNORM;
	case Format::kBC7Unorm: return DXGI_FORMAT_BC7_UNORM;
	case Format::kBC7Srgb: return DXGI_FORMAT_BC7_UNORM_SRGB;
	case Format::kR32Sfloat: return DXGI_FORMAT_R32_FLOAT;
	case Format::kR32G32B32A32Sfloat: return DXGI_FORMAT_R32G32B32A32_FLOAT;
	case Format::kR16G16Sfloat: return DXGI_FORMAT_R16G16_FLOAT;
	case Format::kR32G32Sfloat: return DXGI_FORMAT_R32G32_FLOAT;
	case Format::kR32G32Uint: return DXGI_FORMAT_R32G32_UINT;
	case Format::kR8G8B8Unorm:
	case Format::kB8G8R8Unorm:
	case Format::kA2R10G10B10UnormPack32:
		break;
	}
	ENSUREF(false, "format {} has no DXGI format (24 bit color, or blue in a packed format's low bits)", std::to_underlying(format));
	return DXGI_FORMAT_UNKNOWN;
}

D3D12_BARRIER_SYNC ToD3D12Sync(PipelineStage stages)
{
	auto sync = D3D12_BARRIER_SYNC_NONE;
	auto add = [&sync, stages](PipelineStage stage, D3D12_BARRIER_SYNC value)
	{
		if (Any(stages & stage))
			sync |= value;
	};
	// the top of the pipe, and the host's (its reads and writes are ordered by fences): none
	add(PipelineStage::kTransfer, D3D12_BARRIER_SYNC_COPY | D3D12_BARRIER_SYNC_RESOLVE | D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW);
	add(PipelineStage::kVertexShader, D3D12_BARRIER_SYNC_VERTEX_SHADING);
	add(PipelineStage::kFragmentShader, D3D12_BARRIER_SYNC_PIXEL_SHADING);
	add(PipelineStage::kComputeShader, D3D12_BARRIER_SYNC_COMPUTE_SHADING);
	add(PipelineStage::kColorAttachmentOutput, D3D12_BARRIER_SYNC_RENDER_TARGET);
	add(PipelineStage::kAllGraphics, D3D12_BARRIER_SYNC_DRAW);
	add(PipelineStage::kAllCommands, D3D12_BARRIER_SYNC_ALL);
	add(PipelineStage::kBottomOfPipe, D3D12_BARRIER_SYNC_ALL);
	add(PipelineStage::kEarlyFragmentTests, D3D12_BARRIER_SYNC_DEPTH_STENCIL);
	add(PipelineStage::kLateFragmentTests, D3D12_BARRIER_SYNC_DEPTH_STENCIL);
	add(PipelineStage::kDrawIndirect, D3D12_BARRIER_SYNC_EXECUTE_INDIRECT);
	add(PipelineStage::kAccelerationStructureBuild,
		D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE | D3D12_BARRIER_SYNC_COPY_RAYTRACING_ACCELERATION_STRUCTURE |
			D3D12_BARRIER_SYNC_EMIT_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO);
	return sync;
}

D3D12_BARRIER_ACCESS ToD3D12Access(Access access)
{
	// D3D12's access 0 is COMMON (any): none is NO_ACCESS
	auto result = D3D12_BARRIER_ACCESS_COMMON;
	auto add = [&result, access](Access flag, D3D12_BARRIER_ACCESS value)
	{
		if (Any(access & flag))
			result |= value;
	};
	// a shader's reads of a buffer or an image may be through either view (Vulkan's storage reads are shader reads)
	add(Access::kShaderRead, D3D12_BARRIER_ACCESS_SHADER_RESOURCE | D3D12_BARRIER_ACCESS_CONSTANT_BUFFER | D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
	add(Access::kShaderWrite, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
	add(Access::kTransferRead, D3D12_BARRIER_ACCESS_COPY_SOURCE);
	add(Access::kTransferWrite, D3D12_BARRIER_ACCESS_COPY_DEST);
	add(Access::kIndexRead, D3D12_BARRIER_ACCESS_INDEX_BUFFER);
	add(Access::kColorAttachmentRead, D3D12_BARRIER_ACCESS_RENDER_TARGET);
	add(Access::kColorAttachmentWrite, D3D12_BARRIER_ACCESS_RENDER_TARGET);
	add(Access::kDepthStencilAttachmentRead, D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ);
	add(Access::kDepthStencilAttachmentWrite, D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE);
	add(Access::kIndirectCommandRead, D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
	add(Access::kAccelerationStructureRead, D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);
	add(Access::kAccelerationStructureWrite, D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE);
	return result == D3D12_BARRIER_ACCESS_COMMON ? D3D12_BARRIER_ACCESS_NO_ACCESS : result;
}

D3D12_BARRIER_LAYOUT ToD3D12Layout(ImageLayout layout)
{
	switch (layout)
	{
	case ImageLayout::kUndefined: return D3D12_BARRIER_LAYOUT_UNDEFINED;
	// Vulkan's general layout is where the renderer writes storage images
	case ImageLayout::kGeneral: return D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;
	case ImageLayout::kColorAttachment: return D3D12_BARRIER_LAYOUT_RENDER_TARGET;
	case ImageLayout::kDepthStencilAttachment: return D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE;
	case ImageLayout::kShaderReadOnly: return D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
	case ImageLayout::kTransferSource: return D3D12_BARRIER_LAYOUT_COPY_SOURCE;
	case ImageLayout::kTransferDestination: return D3D12_BARRIER_LAYOUT_COPY_DEST;
	case ImageLayout::kPresent: return D3D12_BARRIER_LAYOUT_PRESENT;
	}
	return D3D12_BARRIER_LAYOUT_COMMON;
}

D3D12_COMMAND_LIST_TYPE ToD3D12(uint32_t queueFamilyIndex)
{
	switch (queueFamilyIndex)
	{
	case 0: return D3D12_COMMAND_LIST_TYPE_DIRECT;
	case 1: return D3D12_COMMAND_LIST_TYPE_COMPUTE;
	case 2: return D3D12_COMMAND_LIST_TYPE_COPY;
	default: break;
	}
	ENSUREF(false, "the D3D12 backend has 3 queue families (direct, compute, copy), not {}", queueFamilyIndex + 1);
	return D3D12_COMMAND_LIST_TYPE_DIRECT;
}

bool RayQueriesEnabled()
{
	const char* value = std::getenv("SPEEDO_RAY_QUERY");
	return value == nullptr || std::string_view(value) != "0";
}

} // namespace rhi::d3d12
