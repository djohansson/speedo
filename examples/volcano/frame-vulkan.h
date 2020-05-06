#pragma once

extern template RenderTarget<FrameCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>::RenderTarget(
    RenderTarget<FrameCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>&& other);

extern template RenderTarget<FrameCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>::RenderTarget(CreateDescType&& desc);

extern template RenderTarget<FrameCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>::~RenderTarget();