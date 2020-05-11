extern template RenderTargetImpl<FrameCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>::RenderTargetImpl(
    RenderTargetImpl<FrameCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>&& other);

extern template RenderTargetImpl<FrameCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>::RenderTargetImpl(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    FrameCreateDesc<GraphicsBackend::Vulkan>&& desc);

extern template RenderTargetImpl<FrameCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>::~RenderTargetImpl();
