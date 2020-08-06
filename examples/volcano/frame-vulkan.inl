extern template RenderTargetImpl<FrameCreateDesc<Vk>, Vk>::RenderTargetImpl(
    RenderTargetImpl<FrameCreateDesc<Vk>, Vk>&& other);

// extern template RenderTargetImpl<FrameCreateDesc<Vk>, Vk>::RenderTargetImpl(
//     const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
//     FrameCreateDesc<Vk>&& desc);

extern template RenderTargetImpl<FrameCreateDesc<Vk>, Vk>::~RenderTargetImpl();

extern template RenderTargetImpl<FrameCreateDesc<Vk>, Vk>& RenderTargetImpl<FrameCreateDesc<Vk>, Vk>::operator=(
    RenderTargetImpl<FrameCreateDesc<Vk>, Vk>&& other);
