template <typename CreateDescType>
class RenderTargetImpl<CreateDescType, Vk> : public RenderTarget<Vk>
{
public:

    virtual ~RenderTargetImpl();

    virtual const RenderTargetCreateDesc<Vk>& getRenderTargetDesc() const final { return myDesc; };

    const auto& getDesc() const { return myDesc; }

protected:

	RenderTargetImpl(
        const std::shared_ptr<DeviceContext<Vk>>& deviceContext,
        CreateDescType&& desc);
    RenderTargetImpl(RenderTargetImpl<CreateDescType, Vk>&& other);

    RenderTargetImpl<CreateDescType, Vk>& operator=(RenderTargetImpl<CreateDescType, Vk>&& other);

private:

    CreateDescType myDesc = {};
};
