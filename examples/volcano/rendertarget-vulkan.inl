template <typename CreateDescType>
class RenderTargetImpl<CreateDescType, Vk> : public RenderTarget<Vk>
{
public:

    virtual const RenderTargetCreateDesc<Vk>& getRenderTargetDesc() const final { return myDesc; };

    const auto& getDesc() const { return myDesc; }

protected:

	RenderTargetImpl(RenderTargetImpl<CreateDescType, Vk>&& other);
	RenderTargetImpl(
        const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
        CreateDescType&& desc);
	virtual ~RenderTargetImpl() = default;

    RenderTargetImpl& operator=(RenderTargetImpl&& other) = default;

private:

    const CreateDescType myDesc = {};
};

template <typename CreateDescType>
RenderTargetImpl<CreateDescType, Vk>::RenderTargetImpl(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
    CreateDescType&& desc)
: RenderTarget<Vk>(deviceContext, desc)
, myDesc(std::move(desc))
{
}

extern template RenderTargetImpl<RenderTargetCreateDesc<Vk>, Vk>::RenderTargetImpl(
    const std::shared_ptr<DeviceContext<Vk>>& deviceContext, RenderTargetCreateDesc<Vk>&& desc);

extern template RenderTargetImpl<RenderTargetCreateDesc<Vk>, Vk>::~RenderTargetImpl();
