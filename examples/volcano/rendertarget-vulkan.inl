template <typename CreateDescType>
class RenderTargetImpl<CreateDescType, GraphicsBackend::Vulkan> : public RenderTarget<GraphicsBackend::Vulkan>
{
    using BaseType = RenderTarget<GraphicsBackend::Vulkan>;

public:

	RenderTargetImpl(RenderTargetImpl<CreateDescType, GraphicsBackend::Vulkan>&& other);
	RenderTargetImpl(CreateDescType&& desc);
	virtual ~RenderTargetImpl();

    const auto& getDesc() const { return myDesc; }

protected:

    const CreateDescType myDesc = {};
};

template <typename CreateDescType>
RenderTargetImpl<CreateDescType, GraphicsBackend::Vulkan>::RenderTargetImpl(RenderTargetImpl<CreateDescType, GraphicsBackend::Vulkan>&& other)
: BaseType(std::move(other), &myDesc)
, myDesc(std::move(other.myDesc))
{
}

template <typename CreateDescType>
RenderTargetImpl<CreateDescType, GraphicsBackend::Vulkan>::RenderTargetImpl(CreateDescType&& desc)
: BaseType(desc, &myDesc)
, myDesc(std::move(desc))
{
}

template <typename CreateDescType>
RenderTargetImpl<CreateDescType, GraphicsBackend::Vulkan>::~RenderTargetImpl()
{
}

extern template RenderTargetImpl<RenderTargetCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>::RenderTargetImpl(
    RenderTargetImpl<RenderTargetCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>&& other);

extern template RenderTargetImpl<RenderTargetCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>::RenderTargetImpl(
    RenderTargetCreateDesc<GraphicsBackend::Vulkan>&& desc);

extern template RenderTargetImpl<RenderTargetCreateDesc<GraphicsBackend::Vulkan>, GraphicsBackend::Vulkan>::~RenderTargetImpl();
