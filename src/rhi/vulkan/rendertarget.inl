template <typename CreateDescType>
class RenderTargetImpl<CreateDescType, Vk> : public RenderTarget<Vk>
{
public:
	virtual ~RenderTargetImpl();

	virtual const RenderTargetCreateDesc<Vk>& getRenderTargetDesc() const final { return myDesc; };

	const auto& getDesc() const { return myDesc; }

protected:
	RenderTargetImpl(
		const std::shared_ptr<Device<Vk>>& device, CreateDescType&& desc);
	RenderTargetImpl(RenderTargetImpl&& other) noexcept;

	RenderTargetImpl<CreateDescType, Vk>& operator=(RenderTargetImpl&& other) noexcept;

	void swap(RenderTargetImpl& rhs) noexcept;

private:
	CreateDescType myDesc{};
};
