template <typename CreateDescType>
class RenderTargetImpl<CreateDescType, kVk> : public RenderTarget<kVk>
{
public:
	~RenderTargetImpl() override;

	[[nodiscard]] const RenderTargetCreateDesc<kVk>& GetRenderTargetDesc() const final
	{
		return myDesc;
	};

	const auto& GetDesc() const { return myDesc; }

	void Swap(RenderTargetImpl& rhs) noexcept;

protected:
	RenderTargetImpl(
		const std::shared_ptr<Device<kVk>>& device, CreateDescType&& desc);
	RenderTargetImpl(RenderTargetImpl&& other) noexcept;

	RenderTargetImpl<CreateDescType, kVk>& operator=(RenderTargetImpl&& other) noexcept;

private:
	CreateDescType myDesc{};
};
