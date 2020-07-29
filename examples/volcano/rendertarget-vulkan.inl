template <typename CreateDescType>
class RenderTargetImpl<CreateDescType, GraphicsBackend::Vulkan> : public RenderTarget<GraphicsBackend::Vulkan>
{
public:

    virtual const RenderTargetCreateDesc<GraphicsBackend::Vulkan>& getRenderTargetDesc() const final { return myDesc; };

    const auto& getDesc() const { return myDesc; }

protected:

	RenderTargetImpl(RenderTargetImpl<CreateDescType, GraphicsBackend::Vulkan>&& other);
	RenderTargetImpl(
        const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
        CreateDescType&& desc);
	virtual ~RenderTargetImpl() = default;

    RenderTargetImpl& operator=(RenderTargetImpl&& other) = default;

private:

    const CreateDescType myDesc = {};
};

template <typename CreateDescType>
RenderTargetImpl<CreateDescType, GraphicsBackend::Vulkan>::RenderTargetImpl(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext,
    CreateDescType&& desc)
: RenderTarget<GraphicsBackend::Vulkan>(deviceContext, desc)
, myDesc(std::move(desc))
{
}

extern template RenderTargetImpl<RenderTargetCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>::RenderTargetImpl(
    const std::shared_ptr<DeviceContext<GraphicsBackend::Vulkan>>& deviceContext, RenderTargetCreateDesc<GraphicsBackend::Vulkan>&& desc);

extern template RenderTargetImpl<RenderTargetCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>::~RenderTargetImpl();
