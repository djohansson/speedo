#pragma once

#include <rhi/buffer.h>
#include <rhi/device.h>
#include <rhi/image.h>
#include <rhi/rhi.h>
#include <rhi/window.h>

// the rhi types gfx uses, for the build's graphics api, so that gfx itself needn't be templated on it
namespace gfx
{

using RHI = rhi::RHI<rhi::kGraphicsApi>;
using Device = rhi::Device<rhi::kGraphicsApi>;
using Buffer = rhi::Buffer<rhi::kGraphicsApi>;
using Image = rhi::Image<rhi::kGraphicsApi>;
using ImageView = rhi::ImageView<rhi::kGraphicsApi>;
using Window = rhi::Window<rhi::kGraphicsApi>;

} // namespace gfx
