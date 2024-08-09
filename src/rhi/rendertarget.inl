template <typename CreateDescType, GraphicsApi G>
class RenderTargetImpl : public RenderTarget<G>
{};

template <GraphicsApi G>
void RenderTarget<G>::AddSubpassDescription(SubpassDescription<G>&& description)
{
	ASSERT(!myRenderInfo.has_value());
	
	mySubPassDescs.emplace_back(std::forward<SubpassDescription<G>>(description));
}

template <GraphicsApi G>
void RenderTarget<G>::AddSubpassDependency(SubpassDependency<G>&& dependency)
{
	ASSERT(!myRenderInfo.has_value());

	mySubPassDependencies.emplace_back(std::forward<SubpassDependency<G>>(dependency));
}

template <GraphicsApi G>
void RenderTarget<G>::ResetSubpasses()
{
	ASSERT(!myRenderInfo.has_value());

	mySubPassDescs.clear();
	mySubPassDependencies.clear();
}

#include "vulkan/rendertarget.inl"
