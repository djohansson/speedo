template <typename CreateDescType>
class RenderTargetImpl<CreateDescType, Vk> : public RenderTarget<Vk>
{
public:

    virtual ~RenderTargetImpl() = default;

    virtual const RenderTargetCreateDesc<Vk>& getRenderTargetDesc() const final { return myDesc; };

    const auto& getDesc() const { return myDesc; }

protected:

	RenderTargetImpl(RenderTargetImpl<CreateDescType, Vk>&& other) = default;
	RenderTargetImpl(
        const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
        CreateDescType&& desc);

    RenderTargetImpl<CreateDescType, Vk>& operator=(RenderTargetImpl<CreateDescType, Vk>&& other) = default;

private:

    const CreateDescType myDesc = {};
};

extern template RenderTargetImpl<RenderTargetCreateDesc<Vk>, Vk>::RenderTargetImpl(
    RenderTargetImpl<RenderTargetCreateDesc<Vk>, Vk>&& other);

extern template RenderTargetImpl<RenderTargetCreateDesc<Vk>, Vk>::~RenderTargetImpl();

extern template RenderTargetImpl<RenderTargetCreateDesc<Vk>, Vk>& RenderTargetImpl<RenderTargetCreateDesc<Vk>, Vk>::operator=(
    RenderTargetImpl<RenderTargetCreateDesc<Vk>, Vk>&& other);
