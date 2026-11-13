// the Metal backend's types (see BackendTypes in rhi/types.h, docs/backends-plan.md): its handles, metal-cpp's classes
// (declared here, defined in the backend's own sources, so that the translation units that include rhi don't take
// Metal.hpp), or where no one class is the handle, an opaque type the backend casts; each a type of its own, since
// rhi's interface tells handles apart by type and compares them as pointers. the structs rhi's classes keep in the
// Vulkan backend's layout are empty placeholders, each its own: where generic code reads one of their fields, the Metal
// build doesn't compile, which shows where rhi's interface still holds Vulkan's structs (the abstraction to correct)
#include <rhi/capi.h>

#include <cstdint>

namespace MTL
{
class AccelerationStructure;
class BinaryArchive;
class Buffer;
class CommandBuffer;
class CommandQueue;
class CounterSampleBuffer;
class Device;
class Library;
class RenderPassDescriptor;
class SamplerState;
class SharedEvent;
class Texture;
} // namespace MTL

namespace CA
{
class MetalLayer;
} // namespace CA

namespace rhi::metal
{

// the opaque handles (what the backend's sources cast them to, in its comment)
struct InstanceObject; // the backend's instance state (Metal has no instance)
struct PhysicalDeviceObject; // an MTL::Device as a candidate
struct AllocatorObject; // the backend's allocator (heaps)
struct AllocationObject; // a resource's allocation
struct BufferViewObject; // an MTL::Texture over a buffer
struct ImageViewObject; // an MTL::Texture view
struct SwapchainObject; // the backend's swapchain (its CAMetalLayer and drawables)
struct FramebufferObject; // none in Metal: the attachments of a render pass
struct DescriptorSetLayoutObject; // an argument buffer's layout
struct DescriptorUpdateTemplateObject; // an argument buffer's encoding
struct PipelineLayoutObject; // the sets' layouts and the push constants
struct DescriptorPoolObject; // the argument buffers' memory
struct DescriptorSetObject; // an argument buffer
struct PipelineObject; // an MTL::RenderPipelineState or MTL::ComputePipelineState
struct CommandPoolObject; // the command buffers' allocator
struct FenceObject; // an MTL::SharedEvent and the value it waits for

// placeholders for the Vulkan backend's structs (see the file's comment)
struct StructureType {};
struct AllocationCallbacks {};
struct ApplicationInfo {};
struct NativeViewport {};
struct NativeRect {};
struct ObjectType {};
struct SurfaceFormat {};
struct PresentMode {};
struct SurfaceCapabilities {};
struct QueueFamilyProperties {};
struct AttachmentDescription {};
struct DescriptorSetLayoutBinding {};
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
struct DescriptorUpdateTemplateEntry {};
struct DescriptorUpdateTemplateType {};
struct PipelineShaderStageCreateInfo {};
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
struct RenderingInfo {};
struct ImageSubresourceLayers {};
struct DescriptorType {};
struct DescriptorBufferInfo {};
struct DescriptorImageInfo {};
struct PushConstantRange {};
struct PipelineRenderingCreateInfo {};

} // namespace rhi::metal

template <>
struct BackendTypes<kMetal>
{
	using StructureType = rhi::metal::StructureType;
	using AllocationCallbacks = rhi::metal::AllocationCallbacks;
	using ApplicationInfo = rhi::metal::ApplicationInfo;
	using Result = int32_t; // an NS::Error's code (0: none)
	using Flags = uint32_t;
	using DeviceSize = uint64_t;
	using NativeViewport = rhi::metal::NativeViewport;
	using NativeRect = rhi::metal::NativeRect;
	using ObjectType = rhi::metal::ObjectType;
	using InstanceHandle = rhi::metal::InstanceObject*;
	using BufferHandle = MTL::Buffer*;
	using ImageHandle = MTL::Texture*;
	using AllocatorHandle = rhi::metal::AllocatorObject*;
	using AllocationHandle = rhi::metal::AllocationObject*;
	using BufferViewHandle = rhi::metal::BufferViewObject*;
	using ImageViewHandle = rhi::metal::ImageViewObject*;
	using SurfaceHandle = CA::MetalLayer*;
	using SurfaceFormat = rhi::metal::SurfaceFormat;
	using PresentMode = rhi::metal::PresentMode;
	using SurfaceCapabilities = rhi::metal::SurfaceCapabilities;
	using QueueFamilyProperties = rhi::metal::QueueFamilyProperties;
	using SwapchainHandle = rhi::metal::SwapchainObject*;
	using FramebufferHandle = rhi::metal::FramebufferObject*;
	using AttachmentDescription = rhi::metal::AttachmentDescription;
	using DescriptorSetLayoutBinding = rhi::metal::DescriptorSetLayoutBinding;
	using DescriptorSetLayoutCreateFlags = uint32_t;
	using DeviceHandle = MTL::Device*;
	using PhysicalDeviceHandle = rhi::metal::PhysicalDeviceObject*;
	using PhysicalDeviceProperties = rhi::metal::PhysicalDeviceProperties;
	using PhysicalDeviceProperties11Ex = rhi::metal::PhysicalDeviceProperties11Ex;
	using PhysicalDeviceProperties12Ex = rhi::metal::PhysicalDeviceProperties12Ex;
	using PhysicalDeviceProperties13Ex = rhi::metal::PhysicalDeviceProperties13Ex;
	using PhysicalDeviceProperties14Ex = rhi::metal::PhysicalDeviceProperties14Ex;
	using PhysicalDeviceFeatures = rhi::metal::PhysicalDeviceFeatures;
	using PhysicalDeviceFeatures11Ex = rhi::metal::PhysicalDeviceFeatures11Ex;
	using PhysicalDeviceFeatures12Ex = rhi::metal::PhysicalDeviceFeatures12Ex;
	using PhysicalDeviceFeatures13Ex = rhi::metal::PhysicalDeviceFeatures13Ex;
	using PhysicalDeviceFeatures14Ex = rhi::metal::PhysicalDeviceFeatures14Ex;
	using PhysicalDevicePresentIdFeatures = rhi::metal::PhysicalDevicePresentIdFeatures;
	using PhysicalDevicePresentWaitFeatures = rhi::metal::PhysicalDevicePresentWaitFeatures;
	using PhysicalDeviceSwapchainMaintenance1Features = rhi::metal::PhysicalDeviceSwapchainMaintenance1Features;
	using PhysicalDeviceFaultFeatures = rhi::metal::PhysicalDeviceFaultFeatures;
	using PhysicalDeviceAddressBindingReportFeatures = rhi::metal::PhysicalDeviceAddressBindingReportFeatures;
	using PhysicalDeviceCoherentMemoryFeatures = rhi::metal::PhysicalDeviceCoherentMemoryFeatures;
	using PhysicalDeviceAccelerationStructureFeatures = rhi::metal::PhysicalDeviceAccelerationStructureFeatures;
	using PhysicalDeviceRayQueryFeatures = rhi::metal::PhysicalDeviceRayQueryFeatures;
	using PhysicalDeviceAccelerationStructureProperties = rhi::metal::PhysicalDeviceAccelerationStructureProperties;
	using QueueHandle = MTL::CommandQueue*;
	using CommandBufferHandle = MTL::CommandBuffer*;
	using ShaderModuleHandle = MTL::Library*;
	using DescriptorUpdateTemplateEntry = rhi::metal::DescriptorUpdateTemplateEntry;
	using DescriptorUpdateTemplateType = rhi::metal::DescriptorUpdateTemplateType;
	using DescriptorSetLayoutHandle = rhi::metal::DescriptorSetLayoutObject*;
	using DescriptorUpdateTemplateHandle = rhi::metal::DescriptorUpdateTemplateObject*;
	using PipelineLayoutHandle = rhi::metal::PipelineLayoutObject*;
	using DescriptorPoolHandle = rhi::metal::DescriptorPoolObject*;
	using DescriptorSetHandle = rhi::metal::DescriptorSetObject*;
	using RenderPassHandle = MTL::RenderPassDescriptor*;
	using PipelineHandle = rhi::metal::PipelineObject*;
	using PipelineCacheHandle = MTL::BinaryArchive*;
	using PipelineShaderStageCreateInfo = rhi::metal::PipelineShaderStageCreateInfo;
	using PipelineVertexInputStateCreateInfo = rhi::metal::PipelineVertexInputStateCreateInfo;
	using PipelineInputAssemblyStateCreateInfo = rhi::metal::PipelineInputAssemblyStateCreateInfo;
	using PipelineViewportStateCreateInfo = rhi::metal::PipelineViewportStateCreateInfo;
	using PipelineRasterizationStateCreateInfo = rhi::metal::PipelineRasterizationStateCreateInfo;
	using PipelineMultisampleStateCreateInfo = rhi::metal::PipelineMultisampleStateCreateInfo;
	using PipelineDepthStencilStateCreateInfo = rhi::metal::PipelineDepthStencilStateCreateInfo;
	using PipelineColorBlendAttachmentState = rhi::metal::PipelineColorBlendAttachmentState;
	using PipelineColorBlendStateCreateInfo = rhi::metal::PipelineColorBlendStateCreateInfo;
	using PipelineDynamicStateCreateInfo = rhi::metal::PipelineDynamicStateCreateInfo;
	using DynamicState = rhi::metal::DynamicState;
	using SamplerHandle = MTL::SamplerState*;
	using CommandPoolCreateFlags = uint32_t;
	using CommandPoolHandle = rhi::metal::CommandPoolObject*;
	using FenceHandle = rhi::metal::FenceObject*;
	using QueryPoolHandle = MTL::CounterSampleBuffer*;
	using SemaphoreHandle = MTL::SharedEvent*;
	using ImageSubresourceRange = rhi::metal::ImageSubresourceRange;
	using CommandBufferInheritanceRenderingInfo = rhi::metal::CommandBufferInheritanceRenderingInfo;
	using RenderPassBeginInfo = rhi::metal::RenderPassBeginInfo;
	using RenderingInfo = rhi::metal::RenderingInfo;
	using ImageSubresourceLayers = rhi::metal::ImageSubresourceLayers;
	using DescriptorType = rhi::metal::DescriptorType;
	using DescriptorBufferInfo = rhi::metal::DescriptorBufferInfo;
	using DescriptorImageInfo = rhi::metal::DescriptorImageInfo;
	using AccelerationStructureHandle = MTL::AccelerationStructure*;
	using PushConstantRange = rhi::metal::PushConstantRange;
	using DescriptorBindingFlags = uint32_t;
	using PipelineRenderingCreateInfo = rhi::metal::PipelineRenderingCreateInfo;
};
