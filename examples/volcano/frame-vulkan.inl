extern template RenderTargetImpl<FrameCreateDesc<Vk>, Vk>::RenderTargetImpl(
    RenderTargetImpl<FrameCreateDesc<Vk>, Vk>&& other);

extern template RenderTargetImpl<FrameCreateDesc<Vk>, Vk>::~RenderTargetImpl();

extern template RenderTargetImpl<FrameCreateDesc<Vk>, Vk>& RenderTargetImpl<FrameCreateDesc<Vk>, Vk>::operator=(
    RenderTargetImpl<FrameCreateDesc<Vk>, Vk>&& other);
