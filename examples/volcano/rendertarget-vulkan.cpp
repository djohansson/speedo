#include "rendertarget.h"

template RenderTarget<RenderTargetCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>::RenderTarget(
    RenderTarget<RenderTargetCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>&& other);

template RenderTarget<RenderTargetCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>::RenderTarget(CreateDescType&& desc);

template RenderTarget<RenderTargetCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>::~RenderTarget();
