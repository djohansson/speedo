#pragma once

// the D3D12 backend's own objects behind its opaque handles (rhi/d3d12/types.inl), for its sources alone: the one place
// that includes <windows.h>, D3D12's and DXGI's headers (DirectX-Headers' d3d12.h, the one D3D12MA is built with)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <directx/d3d12.h>
#include <dxgi1_6.h>

#include <D3D12MemAlloc.h>

#include <rhi/shaderset.h>
#include <rhi/types.h>

#include <core/assert.h>

#include <cstdint>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rhi::d3d12
{

// an HRESULT that must succeed: fails hard naming what failed and the result
void Check(HRESULT result, std::string_view what);

// a COM object released and nulled (where it is one)
template <typename T>
void Release(T*& object) noexcept
{
	if (object != nullptr)
	{
		object->Release();
		object = nullptr;
	}
}

// a name for the debug layer's messages, PIX and DRED (UTF-8, as rhi's names are)
void SetName(ID3D12Object* object, std::string_view name);

// the instance: the DXGI factory, made with DXGI's debug layer where D3D12's is on (SPEEDO_GRAPHICS_VALIDATION_LEVEL, or
// SPEEDO_D3D12_VALIDATE=1, which assettest's -Validate sets)
struct InstanceObject
{
	IDXGIFactory6* factory = nullptr;
	bool validation = false; // D3D12's debug layer enabled, before the devices were made
};

// a heap of descriptors only the cpu sees, a descriptor at a time from a free list: image views', samplers', render
// targets' and depth stencils', copied into the shader visible heaps where a set is written (CopyDescriptorsSimple). pages
// of kPageSize, a page more where they're all taken (the cpu's copies have no limit of D3D12's)
class CpuDescriptorHeap
{
public:
	static constexpr uint32_t kPageSize = 1024;

	void Init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type);
	void Destroy() noexcept;

	[[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE Allocate();
	void Free(D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept;

private:
	std::mutex myMutex;
	ID3D12Device* myDevice = nullptr;
	D3D12_DESCRIPTOR_HEAP_TYPE myType{};
	uint32_t myIncrement = 0;
	std::vector<ID3D12DescriptorHeap*> myPages;
	std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> myFree;
};

// a shader visible heap (CBV, SRV and UAV, or samplers: a command list sets one of each), its descriptors handed out in
// ranges, a descriptor set's tables or a pushed set's (first fit, freed ranges merged with their neighbours). a fixed
// capacity, D3D12's (a million resources' descriptors at resource binding tier 3, 2048 samplers), which fails hard when
// it runs out
class GpuDescriptorHeap
{
public:
	static constexpr uint32_t kNone = ~0U;

	void Init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t capacity, const char* name);
	void Destroy() noexcept;

	// count descriptors' offset in the heap (count 0: kNone)
	[[nodiscard]] uint32_t Allocate(uint32_t count);
	void Free(uint32_t offset, uint32_t count) noexcept;

	[[nodiscard]] ID3D12DescriptorHeap* GetHeap() const noexcept { return myHeap; }
	[[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE Cpu(uint32_t offset) const noexcept
	{
		return {.ptr = myCpuStart.ptr + (size_t{offset} * myIncrement)};
	}
	[[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE Gpu(uint32_t offset) const noexcept
	{
		return {.ptr = myGpuStart.ptr + (uint64_t{offset} * myIncrement)};
	}

private:
	struct Range
	{
		uint32_t offset = 0;
		uint32_t count = 0;
	};

	std::mutex myMutex;
	ID3D12DescriptorHeap* myHeap = nullptr;
	D3D12_DESCRIPTOR_HEAP_TYPE myType{};
	std::string myName;
	uint32_t myCapacity = 0;
	uint32_t myIncrement = 0;
	D3D12_CPU_DESCRIPTOR_HANDLE myCpuStart{};
	D3D12_GPU_DESCRIPTOR_HANDLE myGpuStart{};
	std::vector<Range> myFree; // sorted by offset
};

// the descriptor sets' memory: the device's two shader visible heaps (DescriptorPoolHandle: no pools of D3D12's, every set
// takes its ranges from these)
struct DescriptorPoolObject
{
	GpuDescriptorHeap resources; // CBV, SRV and UAV
	GpuDescriptorHeap samplers;
};

// the device's own state behind its allocator handle (Device::GetAllocator): D3D12MA's allocator and its pool of host
// visible memory, the descriptor heaps, and the command signatures of the indirect commands. a device object finds it
// from its device handle (ForDevice: the device's private data)
struct AllocatorObject
{
	ID3D12Device10* device = nullptr; // not referenced here (the Device's)
	D3D12MA::Allocator* allocator = nullptr;
	// host visible memory that the gpu may write too (a storage buffer the host reads back, as Vulkan's host visible
	// memory may be): a custom heap of the cpu's write back pages in system memory (L0), which unordered access may use,
	// where an upload heap is read only to the gpu and a readback heap a copy's destination alone
	D3D12MA::Pool* hostPool = nullptr;
	CpuDescriptorHeap views; // CBV, SRV and UAV
	CpuDescriptorHeap samplers;
	CpuDescriptorHeap renderTargets;
	CpuDescriptorHeap depthStencils;
	DescriptorPoolObject pool;
	ID3D12CommandSignature* dispatchSignature = nullptr; // DispatchIndirect's
	ID3D12CommandSignature* drawIndexedSignature = nullptr; // DrawIndexedIndirect's (rhi's commands are D3D12's arguments)

	// the allocator of a device made by the backend
	[[nodiscard]] static AllocatorObject& ForDevice(ID3D12Device* device);
};

// a device's allocator registered as its private data, for ForDevice (the device's constructor)
void RegisterAllocator(ID3D12Device* device, AllocatorObject* allocator);

// a descriptor set layout's flag (DescriptorSetLayoutCreateDesc::flags): its descriptors pushed at each bind
inline constexpr uint32_t kDescriptorSetLayoutPush = 1U;

// a descriptor set layout: its bindings' ranges in the set's two descriptor tables, the CBV, SRV and UAV one and the
// samplers' (a table holds one heap's descriptors), each at the registers the code has them at in the set's space
struct DescriptorSetLayoutObject
{
	struct Binding
	{
		uint32_t binding = 0;
		rhi::DescriptorType type{};
		uint32_t count = 0;
		uint32_t shaderRegister = 0;
		uint32_t stride = 0;
		bool writable = false;
		bool sampler = false; // in the samplers' table
		uint32_t offset = 0; // in its table, in descriptors
	};
	std::vector<Binding> bindings; // by binding
	uint32_t space = 0;
	uint32_t resourceCount = 0; // the CBV, SRV and UAV table's descriptors
	uint32_t samplerCount = 0;
	bool push = false;

	[[nodiscard]] const Binding& Find(uint32_t binding) const;
};

// a descriptor set: its tables' ranges in the shader visible heaps (GpuDescriptorHeap::kNone for a table the layout
// hasn't), an array of sets' (DescriptorSetArray: one range of the array's sets, each its part)
struct DescriptorSetObject
{
	uint32_t resources = GpuDescriptorHeap::kNone;
	uint32_t samplers = GpuDescriptorHeap::kNone;
	uint32_t resourceCount = 0; // the layout's, what the set frees
	uint32_t samplerCount = 0;
};

// a pipeline layout: a root signature (version 1.1) of the push constants (root constants), the global uniforms' implicit
// constant buffer (the specialization constant: a root constant, set from the bound pipeline's variant) and each set's
// tables, and where each is among its parameters (kNone: not in it)
struct PipelineLayoutObject
{
	static constexpr uint32_t kNone = ~0U;

	struct SetTables
	{
		uint32_t resources = kNone;
		uint32_t samplers = kNone;
	};

	ID3D12RootSignature* rootSignature = nullptr;
	uint32_t pushConstants = kNone; // the root parameter's index
	uint32_t pushConstantCount = 0; // in 32 bit values
	uint32_t globals = kNone;
	std::vector<SetTables> sets; // by set index
};

// a pipeline: its pipeline state, and what its binding sets besides: the specialization (the variant's, the globals'
// root constant), and a graphics one's primitive topology (a draw's, where the state has its class alone)
struct PipelineObject
{
	ID3D12PipelineState* state = nullptr;
	ID3D12RootSignature* rootSignature = nullptr; // its layout's (not referenced here)
	const PipelineLayoutObject* layout = nullptr; // where the root signature has the sets' tables and the push constants
	uint32_t globals = PipelineLayoutObject::kNone;
	uint32_t specialization = 0;
	D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	bool compute = false;
};

// a shader module: its entry point's DXIL (a container DXC signed)
struct ShaderModuleObject
{
	std::string name; // the entry point's
	std::vector<char> code;
};

// a sampler: its descriptor in the device's cpu heap of samplers
struct SamplerObject
{
	D3D12_CPU_DESCRIPTOR_HANDLE descriptor{};
};

// a command pool: its queue's type, and its command buffers (see command.cpp)
struct CommandPoolObject
{
	ID3D12Device10* device = nullptr;
	D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT;
};

// a command buffer: a command allocator of its own and a command list (D3D12's allocators record one list at a time, and
// a pool's command buffers may record at once), reset when its pool is (its commands done), and what the pipeline bound
// on it, set on the list at the next dispatch or draw: rhi's binds and its commands meet only through the handle
struct CommandBufferObject
{
	ID3D12CommandAllocator* allocator = nullptr;
	ID3D12GraphicsCommandList7* list = nullptr;
	D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	AllocatorObject* device = nullptr; // the device's state (descriptor heaps, command signatures)
	bool recording = false;
	// bound for the next dispatch or draw (none on the list until then)
	const PipelineObject* pipeline = nullptr;
	ID3D12RootSignature* computeRootSignature = nullptr; // the list's, as set
	ID3D12RootSignature* graphicsRootSignature = nullptr;
	const PipelineLayoutObject* layout = nullptr; // the bound sets' and push constants' layout (the pipeline's binds)
	std::vector<uint32_t> pushConstants; // in 32 bit values
	struct SetBinding
	{
		D3D12_GPU_DESCRIPTOR_HANDLE resources{};
		D3D12_GPU_DESCRIPTOR_HANDLE samplers{};
	};
	std::vector<SetBinding> sets; // by set index
	// what a pushed set took of the shader visible heaps, and the upload memory of UpdateBuffer and FillBuffer: freed when
	// the pool is reset (the commands are done)
	std::vector<std::pair<uint32_t, uint32_t>> pushedResources; // offset, count
	std::vector<std::pair<uint32_t, uint32_t>> pushedSamplers;
	std::vector<D3D12MA::Allocation*> transients;
	std::vector<ID3D12Resource*> transientResources;

	// what the pipeline bound, set on the list before a dispatch (compute) or a draw
	void PrepareDispatch();
	void PrepareDraw();
	// the transients freed, for the command buffer's next use
	void FreeTransients() noexcept;
};

// a fence: a D3D12 fence and the value it waits for
struct FenceObject
{
	ID3D12Fence* fence = nullptr;
	uint64_t value = 0;
};

// an image: its resource and allocation, and what its views are made from
struct ImageObject
{
	ID3D12Resource* resource = nullptr;
	D3D12MA::Allocation* allocation = nullptr; // none where it isn't the image's (a swapchain's buffer, a placed one)
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	D3D12_RESOURCE_DIMENSION dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	uint32_t levelCount = 1;
};

// an image view: its descriptors in the device's cpu heaps, those its image's usage allows (ptr 0: none)
struct ImageViewObject
{
	ImageObject* image = nullptr;
	D3D12_CPU_DESCRIPTOR_HANDLE shaderResource{};
	D3D12_CPU_DESCRIPTOR_HANDLE unorderedAccess{};
	D3D12_CPU_DESCRIPTOR_HANDLE renderTarget{};
	D3D12_CPU_DESCRIPTOR_HANDLE depthStencil{};
};

// rhi's vocabulary as D3D12's
[[nodiscard]] DXGI_FORMAT ToD3D12(Format format);
[[nodiscard]] D3D12_BARRIER_SYNC ToD3D12Sync(PipelineStage stages);
[[nodiscard]] D3D12_BARRIER_ACCESS ToD3D12Access(Access access);
[[nodiscard]] D3D12_BARRIER_LAYOUT ToD3D12Layout(ImageLayout layout);
[[nodiscard]] D3D12_COMMAND_LIST_TYPE ToD3D12(uint32_t queueFamilyIndex); // the backend's families: direct, compute, copy

// whether the application asks for ray queries (SPEEDO_RAY_QUERY isn't 0, as Vulkan's backend's)
[[nodiscard]] bool RayQueriesEnabled();

} // namespace rhi::d3d12
