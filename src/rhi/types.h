#pragma once

#include <rhi/capi.h>
#include <rhi/vocabulary.h>

// the backend's types that rhi's interface names (its handles, and the structs its classes keep in the backend's own
// layout), per graphics api: the build's backend specializes BackendTypes with them (Vulkan's in rhi/vulkan/types.inl,
// included by types.inl), and the aliases below name them, so an api without its own fails to compile where they're
// used. in the global namespace (see "Layering" in docs/notes/layering.md). a type only the backend's sources use isn't
// here: they name the backend's own
template <GraphicsApi G>
struct BackendTypes;

#include "types.inl"

template <GraphicsApi G>
using StructureType = typename BackendTypes<G>::StructureType;
template <GraphicsApi G>
using AllocationCallbacks = typename BackendTypes<G>::AllocationCallbacks;
template <GraphicsApi G>
using ApplicationInfo = typename BackendTypes<G>::ApplicationInfo;
template <GraphicsApi G>
using Result = typename BackendTypes<G>::Result;
template <GraphicsApi G>
using Flags = typename BackendTypes<G>::Flags;
template <GraphicsApi G>
using DeviceSize = typename BackendTypes<G>::DeviceSize;
template <GraphicsApi G>
using NativeViewport = typename BackendTypes<G>::NativeViewport;
template <GraphicsApi G>
using NativeRect = typename BackendTypes<G>::NativeRect;
template <GraphicsApi G>
using ObjectType = typename BackendTypes<G>::ObjectType;
template <GraphicsApi G>
using InstanceHandle = typename BackendTypes<G>::InstanceHandle;
template <GraphicsApi G>
using BufferHandle = typename BackendTypes<G>::BufferHandle;
template <GraphicsApi G>
using ImageHandle = typename BackendTypes<G>::ImageHandle;
template <GraphicsApi G>
using AllocatorHandle = typename BackendTypes<G>::AllocatorHandle;
template <GraphicsApi G>
using AllocationHandle = typename BackendTypes<G>::AllocationHandle;
template <GraphicsApi G>
using BufferViewHandle = typename BackendTypes<G>::BufferViewHandle;
template <GraphicsApi G>
using ImageViewHandle = typename BackendTypes<G>::ImageViewHandle;
template <GraphicsApi G>
using SurfaceHandle = typename BackendTypes<G>::SurfaceHandle;
template <GraphicsApi G>
using SurfaceFormat = typename BackendTypes<G>::SurfaceFormat;
template <GraphicsApi G>
using PresentMode = typename BackendTypes<G>::PresentMode;
template <GraphicsApi G>
using SurfaceCapabilities = typename BackendTypes<G>::SurfaceCapabilities;
template <GraphicsApi G>
using QueueFamilyProperties = typename BackendTypes<G>::QueueFamilyProperties;
template <GraphicsApi G>
using SwapchainHandle = typename BackendTypes<G>::SwapchainHandle;
template <GraphicsApi G>
using FramebufferHandle = typename BackendTypes<G>::FramebufferHandle;
template <GraphicsApi G>
using AttachmentDescription = typename BackendTypes<G>::AttachmentDescription;
template <GraphicsApi G>
using DescriptorSetLayoutBinding = typename BackendTypes<G>::DescriptorSetLayoutBinding;
template <GraphicsApi G>
using DescriptorSetLayoutCreateFlags = typename BackendTypes<G>::DescriptorSetLayoutCreateFlags;
template <GraphicsApi G>
using DeviceHandle = typename BackendTypes<G>::DeviceHandle;
template <GraphicsApi G>
using PhysicalDeviceHandle = typename BackendTypes<G>::PhysicalDeviceHandle;
template <GraphicsApi G>
using PhysicalDeviceProperties = typename BackendTypes<G>::PhysicalDeviceProperties;
template <GraphicsApi G>
using PhysicalDeviceProperties11Ex = typename BackendTypes<G>::PhysicalDeviceProperties11Ex;
template <GraphicsApi G>
using PhysicalDeviceProperties12Ex = typename BackendTypes<G>::PhysicalDeviceProperties12Ex;
template <GraphicsApi G>
using PhysicalDeviceProperties13Ex = typename BackendTypes<G>::PhysicalDeviceProperties13Ex;
template <GraphicsApi G>
using PhysicalDeviceProperties14Ex = typename BackendTypes<G>::PhysicalDeviceProperties14Ex;
template <GraphicsApi G>
using PhysicalDeviceFeatures = typename BackendTypes<G>::PhysicalDeviceFeatures;
template <GraphicsApi G>
using PhysicalDeviceFeatures11Ex = typename BackendTypes<G>::PhysicalDeviceFeatures11Ex;
template <GraphicsApi G>
using PhysicalDeviceFeatures12Ex = typename BackendTypes<G>::PhysicalDeviceFeatures12Ex;
template <GraphicsApi G>
using PhysicalDeviceFeatures13Ex = typename BackendTypes<G>::PhysicalDeviceFeatures13Ex;
template <GraphicsApi G>
using PhysicalDeviceFeatures14Ex = typename BackendTypes<G>::PhysicalDeviceFeatures14Ex;
template <GraphicsApi G>
using PhysicalDevicePresentIdFeatures = typename BackendTypes<G>::PhysicalDevicePresentIdFeatures;
template <GraphicsApi G>
using PhysicalDevicePresentWaitFeatures = typename BackendTypes<G>::PhysicalDevicePresentWaitFeatures;
template <GraphicsApi G>
using PhysicalDeviceSwapchainMaintenance1Features = typename BackendTypes<G>::PhysicalDeviceSwapchainMaintenance1Features;
template <GraphicsApi G>
using PhysicalDeviceFaultFeatures = typename BackendTypes<G>::PhysicalDeviceFaultFeatures;
template <GraphicsApi G>
using PhysicalDeviceAddressBindingReportFeatures = typename BackendTypes<G>::PhysicalDeviceAddressBindingReportFeatures;
template <GraphicsApi G>
using PhysicalDeviceCoherentMemoryFeatures = typename BackendTypes<G>::PhysicalDeviceCoherentMemoryFeatures;
template <GraphicsApi G>
using PhysicalDeviceAccelerationStructureFeatures = typename BackendTypes<G>::PhysicalDeviceAccelerationStructureFeatures;
template <GraphicsApi G>
using PhysicalDeviceRayQueryFeatures = typename BackendTypes<G>::PhysicalDeviceRayQueryFeatures;
template <GraphicsApi G>
using PhysicalDeviceAccelerationStructureProperties = typename BackendTypes<G>::PhysicalDeviceAccelerationStructureProperties;
template <GraphicsApi G>
using QueueHandle = typename BackendTypes<G>::QueueHandle;
template <GraphicsApi G>
using CommandBufferHandle = typename BackendTypes<G>::CommandBufferHandle;
template <GraphicsApi G>
using ShaderModuleHandle = typename BackendTypes<G>::ShaderModuleHandle;
template <GraphicsApi G>
using DescriptorUpdateTemplateEntry = typename BackendTypes<G>::DescriptorUpdateTemplateEntry;
template <GraphicsApi G>
using DescriptorUpdateTemplateType = typename BackendTypes<G>::DescriptorUpdateTemplateType;
template <GraphicsApi G>
using DescriptorSetLayoutHandle = typename BackendTypes<G>::DescriptorSetLayoutHandle;
template <GraphicsApi G>
using DescriptorUpdateTemplateHandle = typename BackendTypes<G>::DescriptorUpdateTemplateHandle;
template <GraphicsApi G>
using PipelineLayoutHandle = typename BackendTypes<G>::PipelineLayoutHandle;
template <GraphicsApi G>
using DescriptorPoolHandle = typename BackendTypes<G>::DescriptorPoolHandle;
template <GraphicsApi G>
using DescriptorSetHandle = typename BackendTypes<G>::DescriptorSetHandle;
template <GraphicsApi G>
using RenderPassHandle = typename BackendTypes<G>::RenderPassHandle;
template <GraphicsApi G>
using PipelineHandle = typename BackendTypes<G>::PipelineHandle;
template <GraphicsApi G>
using PipelineCacheHandle = typename BackendTypes<G>::PipelineCacheHandle;
template <GraphicsApi G>
using PipelineShaderStageCreateInfo = typename BackendTypes<G>::PipelineShaderStageCreateInfo;
template <GraphicsApi G>
using PipelineVertexInputStateCreateInfo = typename BackendTypes<G>::PipelineVertexInputStateCreateInfo;
template <GraphicsApi G>
using PipelineInputAssemblyStateCreateInfo = typename BackendTypes<G>::PipelineInputAssemblyStateCreateInfo;
template <GraphicsApi G>
using PipelineViewportStateCreateInfo = typename BackendTypes<G>::PipelineViewportStateCreateInfo;
template <GraphicsApi G>
using PipelineRasterizationStateCreateInfo = typename BackendTypes<G>::PipelineRasterizationStateCreateInfo;
template <GraphicsApi G>
using PipelineMultisampleStateCreateInfo = typename BackendTypes<G>::PipelineMultisampleStateCreateInfo;
template <GraphicsApi G>
using PipelineDepthStencilStateCreateInfo = typename BackendTypes<G>::PipelineDepthStencilStateCreateInfo;
template <GraphicsApi G>
using PipelineColorBlendAttachmentState = typename BackendTypes<G>::PipelineColorBlendAttachmentState;
template <GraphicsApi G>
using PipelineColorBlendStateCreateInfo = typename BackendTypes<G>::PipelineColorBlendStateCreateInfo;
template <GraphicsApi G>
using PipelineDynamicStateCreateInfo = typename BackendTypes<G>::PipelineDynamicStateCreateInfo;
template <GraphicsApi G>
using DynamicState = typename BackendTypes<G>::DynamicState;
template <GraphicsApi G>
using SamplerHandle = typename BackendTypes<G>::SamplerHandle;
template <GraphicsApi G>
using CommandPoolCreateFlags = typename BackendTypes<G>::CommandPoolCreateFlags;
template <GraphicsApi G>
using CommandPoolHandle = typename BackendTypes<G>::CommandPoolHandle;
template <GraphicsApi G>
using FenceHandle = typename BackendTypes<G>::FenceHandle;
template <GraphicsApi G>
using QueryPoolHandle = typename BackendTypes<G>::QueryPoolHandle;
template <GraphicsApi G>
using SemaphoreHandle = typename BackendTypes<G>::SemaphoreHandle;
template <GraphicsApi G>
using ImageSubresourceRange = typename BackendTypes<G>::ImageSubresourceRange;
template <GraphicsApi G>
using CommandBufferInheritanceRenderingInfo = typename BackendTypes<G>::CommandBufferInheritanceRenderingInfo;
template <GraphicsApi G>
using RenderPassBeginInfo = typename BackendTypes<G>::RenderPassBeginInfo;
template <GraphicsApi G>
using RenderingInfo = typename BackendTypes<G>::RenderingInfo;
template <GraphicsApi G>
using ImageSubresourceLayers = typename BackendTypes<G>::ImageSubresourceLayers;
template <GraphicsApi G>
using DescriptorType = typename BackendTypes<G>::DescriptorType;
template <GraphicsApi G>
using DescriptorBufferInfo = typename BackendTypes<G>::DescriptorBufferInfo;
template <GraphicsApi G>
using DescriptorImageInfo = typename BackendTypes<G>::DescriptorImageInfo;
template <GraphicsApi G>
using AccelerationStructureHandle = typename BackendTypes<G>::AccelerationStructureHandle;
template <GraphicsApi G>
using PushConstantRange = typename BackendTypes<G>::PushConstantRange;
template <GraphicsApi G>
using DescriptorBindingFlags = typename BackendTypes<G>::DescriptorBindingFlags;
template <GraphicsApi G>
using PipelineRenderingCreateInfo = typename BackendTypes<G>::PipelineRenderingCreateInfo;
