#pragma once

#include <gfx/graphicsapi.h>

#include <rhi/accelerationstructure.h>
#include <rhi/buffer.h>
#include <rhi/commandencoder.h>
#include <rhi/crashreport.h>
#include <rhi/device.h>
#include <rhi/image.h>
#include <rhi/pipeline.h>
#include <rhi/querypool.h>
#include <rhi/renderimageset.h>
#include <rhi/rhi.h>
#include <rhi/sampler.h>
#include <rhi/swapchain.h>

// the rhi types gfx uses, for the copy's graphics api (gfx/graphicsapi.h), so that gfx itself needn't be templated on it
namespace gfx::inline SPEEDO_GRAPHICS_API_NAMESPACE
{

using RHI = rhi::RHI<kGraphicsApi>;
using DrawCall = rhi::DrawCall<kGraphicsApi>;
using Device = rhi::Device<kGraphicsApi>;
using Buffer = rhi::Buffer<kGraphicsApi>;
using BufferCreateDesc = rhi::BufferCreateDesc<kGraphicsApi>;
using Image = rhi::Image<kGraphicsApi>;
using ImageCreateDesc = rhi::ImageCreateDesc<kGraphicsApi>;
using ImageView = rhi::ImageView<kGraphicsApi>;
using ImageViewCreateDesc = rhi::ImageViewCreateDesc<kGraphicsApi>;
using Swapchain = rhi::Swapchain<kGraphicsApi>;
using Semaphore = rhi::Semaphore<kGraphicsApi>;
using QueryPool = rhi::QueryPool<kGraphicsApi>;
using QueryPoolCreateDesc = rhi::QueryPoolCreateDesc<kGraphicsApi>;
using GpuProfiler = rhi::GpuProfiler<kGraphicsApi>;
using CrashBreadcrumb = rhi::CrashBreadcrumb<kGraphicsApi>;
using SemaphoreCreateDesc = rhi::SemaphoreCreateDesc<kGraphicsApi>;
using Queue = rhi::Queue<kGraphicsApi>;
using CommandBuffer = rhi::CommandBuffer<kGraphicsApi>;
using QueueDeviceSyncInfo = rhi::QueueDeviceSyncInfo<kGraphicsApi>;
using QueueTimelineContextData = rhi::QueueTimelineContextData<kGraphicsApi>;
using QueueTimelineContext = rhi::QueueTimelineContext<kGraphicsApi>;
using Pipeline = rhi::Pipeline<kGraphicsApi>;
using RenderImageSet = rhi::RenderImageSet<kGraphicsApi>;
using SamplerVector = rhi::SamplerVector<kGraphicsApi>;
using SamplerVectorCreateDesc = rhi::SamplerVectorCreateDesc<kGraphicsApi>;
using CommandEncoder = rhi::CommandEncoder<kGraphicsApi>;
using CommandBufferHandle = ::CommandBufferHandle<kGraphicsApi>;
using SemaphoreHandle = ::SemaphoreHandle<kGraphicsApi>;
using PipelineLayoutHandle = ::PipelineLayoutHandle<kGraphicsApi>;
using ImageViewHandle = ::ImageViewHandle<kGraphicsApi>;
using BufferBinding = rhi::BufferBinding<kGraphicsApi>;
using ImageBinding = rhi::ImageBinding<kGraphicsApi>;
using IRenderTarget = rhi::IRenderTarget<kGraphicsApi>;
using RenderTargetBeginInfo = rhi::RenderTargetBeginInfo<kGraphicsApi>;
using MemoryBlock = rhi::MemoryBlock<kGraphicsApi>;
using AccelerationStructure = rhi::AccelerationStructure<kGraphicsApi>;
using AccelerationStructureCreateDesc = rhi::AccelerationStructureCreateDesc<kGraphicsApi>;

} // namespace gfx::inline SPEEDO_GRAPHICS_API_NAMESPACE
