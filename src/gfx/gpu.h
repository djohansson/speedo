#pragma once

#include <rhi/buffer.h>
#include <rhi/commandencoder.h>
#include <rhi/device.h>
#include <rhi/image.h>
#include <rhi/imguirenderer.h>
#include <rhi/pipeline.h>
#include <rhi/renderimageset.h>
#include <rhi/rhi.h>
#include <rhi/sampler.h>
#include <rhi/window.h>

// the rhi types gfx uses, for the build's graphics api, so that gfx itself needn't be templated on it
namespace gfx
{

using RHI = rhi::RHI<rhi::kGraphicsApi>;
using Device = rhi::Device<rhi::kGraphicsApi>;
using Buffer = rhi::Buffer<rhi::kGraphicsApi>;
using BufferCreateDesc = rhi::BufferCreateDesc<rhi::kGraphicsApi>;
using Image = rhi::Image<rhi::kGraphicsApi>;
using ImageCreateDesc = rhi::ImageCreateDesc<rhi::kGraphicsApi>;
using ImageMipLevelDesc = rhi::ImageMipLevelDesc<rhi::kGraphicsApi>;
using ImageView = rhi::ImageView<rhi::kGraphicsApi>;
using ImageViewCreateDesc = rhi::ImageViewCreateDesc<rhi::kGraphicsApi>;
using Window = rhi::Window<rhi::kGraphicsApi>;
using Semaphore = rhi::Semaphore<rhi::kGraphicsApi>;
using SemaphoreCreateDesc = rhi::SemaphoreCreateDesc<rhi::kGraphicsApi>;
using Queue = rhi::Queue<rhi::kGraphicsApi>;
using QueueDeviceSyncInfo = rhi::QueueDeviceSyncInfo<rhi::kGraphicsApi>;
using QueueTimelineContextData = rhi::QueueTimelineContextData<rhi::kGraphicsApi>;
using QueueTimelineContext = rhi::QueueTimelineContext<rhi::kGraphicsApi>;
using Pipeline = rhi::Pipeline<rhi::kGraphicsApi>;
using RenderImageSet = rhi::RenderImageSet<rhi::kGraphicsApi>;
using SamplerVector = rhi::SamplerVector<rhi::kGraphicsApi>;
using SamplerVectorCreateDesc = rhi::SamplerVectorCreateDesc<rhi::kGraphicsApi>;
using CommandEncoder = rhi::CommandEncoder<rhi::kGraphicsApi>;
using CommandBufferHandle = ::CommandBufferHandle<rhi::kGraphicsApi>;
using SemaphoreHandle = ::SemaphoreHandle<rhi::kGraphicsApi>;
using ImGuiRenderer = rhi::ImGuiRenderer<rhi::kGraphicsApi>;
using BufferBinding = rhi::BufferBinding<rhi::kGraphicsApi>;
using ImageBinding = rhi::ImageBinding<rhi::kGraphicsApi>;

} // namespace gfx
