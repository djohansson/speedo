// the D3D12 backend's types (see BackendTypes in rhi/types.h, docs/backends-plan.md): its handles, D3D12's and DXGI's
// interfaces (declared here, not included: the translation units that include rhi don't take <windows.h> or d3d12.h, see
// docs/notes/build.md) or, where no one interface is the handle, an opaque type the backend casts; each a type of its own,
// since rhi's interface tells handles apart by type (a buffer's resource and an image's are different handles) and
// compares them as pointers. the structs rhi's classes keep in the Vulkan backend's layout are the D3D12 backend's own
// where it uses them, empty placeholders otherwise, each its own (as the Metal backend's, rhi/metal/types.inl)
#include <rhi/capi.h>
#include <rhi/vocabulary.h>

#include <array>
#include <cstdint>

struct ID3D12CommandQueue;
struct ID3D12Device10;
struct ID3D12Fence;
struct ID3D12PipelineLibrary;
struct ID3D12Resource;
struct HWND__;

namespace D3D12MA
{
class Allocation;
} // namespace D3D12MA

namespace rhi::d3d12
{

// the opaque handles (what the backend's sources cast them to, in its comment)
struct InstanceObject; // the DXGI factory and the debug layer's state
struct AllocatorObject; // the device's D3D12MA allocator, its memory pools, descriptor heaps and command signatures
struct PhysicalDeviceObject; // an IDXGIAdapter4 as a candidate
struct ImageObject; // an image's ID3D12Resource and its description
struct BufferViewObject; // a typed buffer's view (descriptors)
struct ImageViewObject; // an image view's descriptors (shader resource, unordered access, render target or depth stencil)
struct SwapchainObject; // the backend's swapchain (its IDXGISwapChain4 and images)
struct FramebufferObject; // none in D3D12: the attachments of a render pass
struct RenderPassObject; // none in D3D12: a render target's attachments are set at its Begin
struct DescriptorSetLayoutObject; // a set's descriptor tables' ranges (resources and samplers)
struct DescriptorUpdateTemplateObject; // none: the entries are the template
struct PipelineLayoutObject; // a root signature, and where each set's tables and the push constants are in it
struct DescriptorPoolObject; // the shader visible descriptor heaps' allocator
struct DescriptorSetObject; // a set's ranges of the shader visible heaps
struct PipelineObject; // an ID3D12PipelineState and what binding it sets besides
struct CommandPoolObject; // the queue's type, and its command allocators and lists
struct CommandBufferObject; // a command allocator and list, and what the pipeline bound on it
struct FenceObject; // an ID3D12Fence and the value it waits for
struct QueryPoolObject; // a query heap and the buffer its queries resolve to
struct ShaderModuleObject; // an entry point's DXIL
struct SamplerObject; // a sampler's descriptor
struct AccelerationStructureObject; // a structure's buffer and its gpu address

// placeholders for the Vulkan backend's structs (see the file's comment)
struct StructureType {};
struct AllocationCallbacks {};
struct ApplicationInfo {};
struct NativeViewport {};
struct NativeRect {};
struct ObjectType {};
struct SurfaceFormat // the swapchain's images' format
{
	rhi::Format format = rhi::Format::kUndefined;
};
enum class PresentMode : uint8_t // a flip model swapchain's: fifo (a sync interval of 1), mailbox (0), immediate (0, tearing)
{
	kFifo,
	kMailbox,
	kImmediate,
};
struct SurfaceCapabilities {};
struct QueueFamilyProperties {};
struct AttachmentDescription {};
struct DescriptorSetLayoutBinding // a binding of a set's layout: its range in one of the set's tables
{
	uint32_t binding = 0;
	rhi::DescriptorType descriptorType{};
	uint32_t descriptorCount = 0;
	uint32_t shaderRegister = 0; // its register in the set's space, the code's (rhi::ShaderBinding::slot)
	uint32_t registerSpace = 0; // the set's space, the code's (rhi::ShaderSetLayout::slot)
	uint32_t stride = 0; // a structured buffer's element's, in bytes (its view's), 0 for a raw buffer or another type
	bool writable = false; // a storage buffer's view: unordered access, else a shader resource
};
struct PhysicalDeviceProperties {};
struct PhysicalDeviceProperties11Ex {};
struct PhysicalDeviceProperties12Ex {};
struct PhysicalDeviceProperties13Ex {};
struct PhysicalDeviceProperties14Ex {};
struct PhysicalDeviceFeatures {};
struct PhysicalDeviceFeatures11Ex {};
struct PhysicalDeviceFeatures12Ex {};
struct PhysicalDeviceFeatures13Ex {};
struct PhysicalDeviceFeatures14Ex {};
struct PhysicalDevicePresentIdFeatures {};
struct PhysicalDevicePresentWaitFeatures {};
struct PhysicalDeviceSwapchainMaintenance1Features {};
struct PhysicalDeviceFaultFeatures {};
struct PhysicalDeviceAddressBindingReportFeatures {};
struct PhysicalDeviceCoherentMemoryFeatures {};
struct PhysicalDeviceAccelerationStructureFeatures {};
struct PhysicalDeviceRayQueryFeatures {};
struct PhysicalDeviceAccelerationStructureProperties {};
struct DescriptorUpdateTemplateEntry // a range of a binding's elements, and where their values are among the set's
{
	uint32_t dstBinding = 0;
	uint32_t dstArrayElement = 0;
	uint32_t descriptorCount = 0;
	rhi::DescriptorType descriptorType{};
	size_t offset = 0; // in bytes, as Vulkan's: the first value's
	size_t stride = 0;
};
enum class DescriptorUpdateTemplateType : uint8_t
{
	kDescriptorSet, // written into a set's ranges of the shader visible heaps (Pipeline::InternalUpdateDescriptorSet)
	kPushDescriptors, // written at each bind into ranges of the command buffer's own (Pipeline::InternalPushDescriptorSet)
};
struct PipelineShaderStageCreateInfo // an entry point: its module (its DXIL) and name
{
	ShaderModuleObject* module = nullptr;
	const char* name = nullptr; // the layout's shader module's
	rhi::ShaderStage stage{};
	std::array<uint64_t, 3> threadGroupSize{1, 1, 1}; // a compute entry point's (its numthreads)
};
struct PipelineVertexInputStateCreateInfo {};
struct PipelineInputAssemblyStateCreateInfo {};
struct PipelineViewportStateCreateInfo {};
struct PipelineRasterizationStateCreateInfo {};
struct PipelineMultisampleStateCreateInfo {};
struct PipelineDepthStencilStateCreateInfo {};
struct PipelineColorBlendAttachmentState {};
struct PipelineColorBlendStateCreateInfo {};
struct PipelineDynamicStateCreateInfo {};
struct DynamicState {};
struct ImageSubresourceRange {};
struct CommandBufferInheritanceRenderingInfo {};
struct RenderPassBeginInfo {};
// a render target's pass as its begin info hands it on: the command buffer recording it, which secondary command buffers
// record their passes' state from (CommandPool::BeginSecondary)
struct RenderingInfo
{
	CommandBufferObject* primary = nullptr;
};
struct ImageSubresourceLayers {};
struct DescriptorBufferInfo // a buffer's view: the resource, and the range of it the shaders see
{
	ID3D12Resource* buffer = nullptr;
	uint64_t offset = 0;
	uint64_t range = 0;

	[[nodiscard]] bool operator==(const DescriptorBufferInfo&) const = default;
};
struct DescriptorImageInfo // a sampler's or an image view's descriptors (no layouts in a descriptor: the barriers set them)
{
	SamplerObject* sampler = nullptr;
	ImageViewObject* imageView = nullptr;

	[[nodiscard]] bool operator==(const DescriptorImageInfo&) const = default;
};
struct PushConstantRange // root constants
{
	rhi::ShaderStage stages{};
	uint32_t offset = 0;
	uint32_t size = 0;
};
struct PipelineRenderingCreateInfo // a render target's attachments' formats, which a graphics pipeline state is made for
{
	uint32_t colorAttachmentCount = 0;
	std::array<rhi::Format, rhi::kMaxColorAttachments> colorAttachmentFormats{};
	rhi::Format depthAttachmentFormat = rhi::Format::kUndefined;
	rhi::Format stencilAttachmentFormat = rhi::Format::kUndefined;
};

} // namespace rhi::d3d12

template <>
struct BackendTypes<kD3D12>
{
	using StructureType = rhi::d3d12::StructureType;
	using AllocationCallbacks = rhi::d3d12::AllocationCallbacks;
	using ApplicationInfo = rhi::d3d12::ApplicationInfo;
	using Result = int32_t; // an HRESULT
	using Flags = uint32_t;
	using DeviceSize = uint64_t;
	using NativeViewport = rhi::d3d12::NativeViewport;
	using NativeRect = rhi::d3d12::NativeRect;
	using ObjectType = rhi::d3d12::ObjectType;
	using InstanceHandle = rhi::d3d12::InstanceObject*;
	using BufferHandle = ID3D12Resource*;
	using ImageHandle = rhi::d3d12::ImageObject*;
	using AllocatorHandle = rhi::d3d12::AllocatorObject*;
	using AllocationHandle = D3D12MA::Allocation*;
	using BufferViewHandle = rhi::d3d12::BufferViewObject*;
	using ImageViewHandle = rhi::d3d12::ImageViewObject*;
	using SurfaceHandle = HWND__*;
	using SurfaceFormat = rhi::d3d12::SurfaceFormat;
	using PresentMode = rhi::d3d12::PresentMode;
	using SurfaceCapabilities = rhi::d3d12::SurfaceCapabilities;
	using QueueFamilyProperties = rhi::d3d12::QueueFamilyProperties;
	using SwapchainHandle = rhi::d3d12::SwapchainObject*;
	using FramebufferHandle = rhi::d3d12::FramebufferObject*;
	using AttachmentDescription = rhi::d3d12::AttachmentDescription;
	using DescriptorSetLayoutBinding = rhi::d3d12::DescriptorSetLayoutBinding;
	using DescriptorSetLayoutCreateFlags = uint32_t;
	using DeviceHandle = ID3D12Device10*;
	using PhysicalDeviceHandle = rhi::d3d12::PhysicalDeviceObject*;
	using PhysicalDeviceProperties = rhi::d3d12::PhysicalDeviceProperties;
	using PhysicalDeviceProperties11Ex = rhi::d3d12::PhysicalDeviceProperties11Ex;
	using PhysicalDeviceProperties12Ex = rhi::d3d12::PhysicalDeviceProperties12Ex;
	using PhysicalDeviceProperties13Ex = rhi::d3d12::PhysicalDeviceProperties13Ex;
	using PhysicalDeviceProperties14Ex = rhi::d3d12::PhysicalDeviceProperties14Ex;
	using PhysicalDeviceFeatures = rhi::d3d12::PhysicalDeviceFeatures;
	using PhysicalDeviceFeatures11Ex = rhi::d3d12::PhysicalDeviceFeatures11Ex;
	using PhysicalDeviceFeatures12Ex = rhi::d3d12::PhysicalDeviceFeatures12Ex;
	using PhysicalDeviceFeatures13Ex = rhi::d3d12::PhysicalDeviceFeatures13Ex;
	using PhysicalDeviceFeatures14Ex = rhi::d3d12::PhysicalDeviceFeatures14Ex;
	using PhysicalDevicePresentIdFeatures = rhi::d3d12::PhysicalDevicePresentIdFeatures;
	using PhysicalDevicePresentWaitFeatures = rhi::d3d12::PhysicalDevicePresentWaitFeatures;
	using PhysicalDeviceSwapchainMaintenance1Features = rhi::d3d12::PhysicalDeviceSwapchainMaintenance1Features;
	using PhysicalDeviceFaultFeatures = rhi::d3d12::PhysicalDeviceFaultFeatures;
	using PhysicalDeviceAddressBindingReportFeatures = rhi::d3d12::PhysicalDeviceAddressBindingReportFeatures;
	using PhysicalDeviceCoherentMemoryFeatures = rhi::d3d12::PhysicalDeviceCoherentMemoryFeatures;
	using PhysicalDeviceAccelerationStructureFeatures = rhi::d3d12::PhysicalDeviceAccelerationStructureFeatures;
	using PhysicalDeviceRayQueryFeatures = rhi::d3d12::PhysicalDeviceRayQueryFeatures;
	using PhysicalDeviceAccelerationStructureProperties = rhi::d3d12::PhysicalDeviceAccelerationStructureProperties;
	using QueueHandle = ID3D12CommandQueue*;
	using CommandBufferHandle = rhi::d3d12::CommandBufferObject*;
	using ShaderModuleHandle = rhi::d3d12::ShaderModuleObject*;
	using DescriptorUpdateTemplateEntry = rhi::d3d12::DescriptorUpdateTemplateEntry;
	using DescriptorUpdateTemplateType = rhi::d3d12::DescriptorUpdateTemplateType;
	using DescriptorSetLayoutHandle = rhi::d3d12::DescriptorSetLayoutObject*;
	using DescriptorUpdateTemplateHandle = rhi::d3d12::DescriptorUpdateTemplateObject*;
	using PipelineLayoutHandle = rhi::d3d12::PipelineLayoutObject*;
	using DescriptorPoolHandle = rhi::d3d12::DescriptorPoolObject*;
	using DescriptorSetHandle = rhi::d3d12::DescriptorSetObject*;
	using RenderPassHandle = rhi::d3d12::RenderPassObject*;
	using PipelineHandle = rhi::d3d12::PipelineObject*;
	using PipelineCacheHandle = ID3D12PipelineLibrary*;
	using PipelineShaderStageCreateInfo = rhi::d3d12::PipelineShaderStageCreateInfo;
	using PipelineVertexInputStateCreateInfo = rhi::d3d12::PipelineVertexInputStateCreateInfo;
	using PipelineInputAssemblyStateCreateInfo = rhi::d3d12::PipelineInputAssemblyStateCreateInfo;
	using PipelineViewportStateCreateInfo = rhi::d3d12::PipelineViewportStateCreateInfo;
	using PipelineRasterizationStateCreateInfo = rhi::d3d12::PipelineRasterizationStateCreateInfo;
	using PipelineMultisampleStateCreateInfo = rhi::d3d12::PipelineMultisampleStateCreateInfo;
	using PipelineDepthStencilStateCreateInfo = rhi::d3d12::PipelineDepthStencilStateCreateInfo;
	using PipelineColorBlendAttachmentState = rhi::d3d12::PipelineColorBlendAttachmentState;
	using PipelineColorBlendStateCreateInfo = rhi::d3d12::PipelineColorBlendStateCreateInfo;
	using PipelineDynamicStateCreateInfo = rhi::d3d12::PipelineDynamicStateCreateInfo;
	using DynamicState = rhi::d3d12::DynamicState;
	using SamplerHandle = rhi::d3d12::SamplerObject*;
	using CommandPoolCreateFlags = uint32_t;
	using CommandPoolHandle = rhi::d3d12::CommandPoolObject*;
	using FenceHandle = rhi::d3d12::FenceObject*;
	using QueryPoolHandle = rhi::d3d12::QueryPoolObject*;
	using SemaphoreHandle = ID3D12Fence*;
	using ImageSubresourceRange = rhi::d3d12::ImageSubresourceRange;
	using CommandBufferInheritanceRenderingInfo = rhi::d3d12::CommandBufferInheritanceRenderingInfo;
	using RenderPassBeginInfo = rhi::d3d12::RenderPassBeginInfo;
	using RenderingInfo = rhi::d3d12::RenderingInfo;
	using ImageSubresourceLayers = rhi::d3d12::ImageSubresourceLayers;
	using DescriptorType = rhi::DescriptorType;
	using DescriptorBufferInfo = rhi::d3d12::DescriptorBufferInfo;
	using DescriptorImageInfo = rhi::d3d12::DescriptorImageInfo;
	using AccelerationStructureHandle = rhi::d3d12::AccelerationStructureObject*;
	using PushConstantRange = rhi::d3d12::PushConstantRange;
	using DescriptorBindingFlags = uint32_t;
	using PipelineRenderingCreateInfo = rhi::d3d12::PipelineRenderingCreateInfo;
};
