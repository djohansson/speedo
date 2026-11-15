// the D3D12 backend's render targets (rhi/d3d12/rendertarget.inl): their attachments' formats, as a graphics pipeline
// state is made for them, and what a pass does with each. the passes themselves come with the render passes (step 3,
// docs/backends-plan.md): until then they fail hard. instantiated here for the backend's render targets (RenderImageSet,
// and the swapchain's frames)
#include <rhi/d3d12/backend.h>

#include <rhi/frame.h>
#include <rhi/renderimageset.h>
#include <rhi/rhi.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <utility>

namespace rhi
{

namespace rendertarget
{

[[noreturn]] static void NotYet()
{
	ENSUREF(false, "the D3D12 backend has no render passes yet (docs/backends-plan.md, step 3)");
	std::abort();
}

} // namespace rendertarget

template <typename DerivedType>
RenderTarget<DerivedType, kD3D12>::RenderTarget(typename ObjectTraits<DerivedType>::CreateDescType&& desc)
	: DeviceObject<DerivedType>(std::forward<typename ObjectTraits<DerivedType>::CreateDescType>(desc))
{
	const auto& formats = this->GetDesc().imageFormats;
	myOps.resize(formats.size());
	myAttachmentDescs.resize(formats.size());
	auto& rendering = myPipelineRenderingCreateInfo.emplace();
	for (auto format : formats)
	{
		if (HasColorComponent(format))
		{
			ENSUREF(rendering.colorAttachmentCount < kMaxColorAttachments, "a render target of more than {} color attachments", kMaxColorAttachments);
			rendering.colorAttachmentFormats[rendering.colorAttachmentCount++] = format;
			continue;
		}
		if (HasDepthComponent(format))
			rendering.depthAttachmentFormat = format;
		if (HasStencilComponent(format))
			rendering.stencilAttachmentFormat = format;
	}
	InternalUpdateAttachments();
}

template <typename DerivedType>
RenderTarget<DerivedType, kD3D12>::RenderTarget(RenderTarget&& other) noexcept
{
	Swap(other);
}

template <typename DerivedType>
RenderTarget<DerivedType, kD3D12>::~RenderTarget()
{
	InternalReleaseAttachments();
}

template <typename DerivedType>
RenderTarget<DerivedType, kD3D12>& RenderTarget<DerivedType, kD3D12>::operator=(RenderTarget&& other) noexcept
{
	Swap(other);
	return *this;
}

template <typename DerivedType>
void RenderTarget<DerivedType, kD3D12>::Swap(RenderTarget& rhs) noexcept
{
	DeviceObject<DerivedType>::Swap(rhs);
	std::swap(myAttachments, rhs.myAttachments);
	std::swap(myAttachmentDescs, rhs.myAttachmentDescs);
	std::swap(myOps, rhs.myOps);
	std::swap(myPipelineRenderingCreateInfo, rhs.myPipelineRenderingCreateInfo);
	std::swap(myBeginInfo, rhs.myBeginInfo);
}

template <typename DerivedType>
void RenderTarget<DerivedType, kD3D12>::InternalReleaseAttachments() noexcept
{
	myAttachments.clear();
}

template <typename DerivedType>
void RenderTarget<DerivedType, kD3D12>::InternalUpdateAttachments()
{
	ENSUREF(this->GetDesc().images.empty(), "the D3D12 backend has no render passes yet (docs/backends-plan.md, step 3)");
}

template <typename DerivedType>
const RenderTargetBeginInfo<kD3D12>& RenderTarget<DerivedType, kD3D12>::Begin(CommandBufferHandle<kD3D12> /*cmd*/, SubpassContents /*contents*/)
{
	rendertarget::NotYet();
}

template <typename DerivedType>
void RenderTarget<DerivedType, kD3D12>::End(CommandBufferHandle<kD3D12> /*cmd*/)
{
	rendertarget::NotYet();
}

template <typename DerivedType>
void RenderTarget<DerivedType, kD3D12>::ClearAll(CommandBufferHandle<kD3D12> /*cmd*/, std::span<const ClearValue> /*values*/) const
{
	rendertarget::NotYet();
}

template <typename DerivedType>
void RenderTarget<DerivedType, kD3D12>::Clear(CommandBufferHandle<kD3D12> /*cmd*/, const ClearValue& /*value*/, uint32_t /*index*/)
{
	rendertarget::NotYet();
}

template <typename DerivedType>
void RenderTarget<DerivedType, kD3D12>::ClearRect(
	CommandBufferHandle<kD3D12> /*cmd*/, const ClearValue& /*value*/, uint32_t /*index*/, const Rect& /*rect*/) const
{
	rendertarget::NotYet();
}

template <typename DerivedType>
void RenderTarget<DerivedType, kD3D12>::Copy(
	CommandBufferHandle<kD3D12> /*cmd*/,
	const IRenderTarget<kD3D12>& /*srcRenderTarget*/,
	const ImageSubresourceLayers<kD3D12>& /*srcSubresource*/,
	uint32_t /*srcIndex*/,
	const ImageSubresourceLayers<kD3D12>& /*dstSubresource*/,
	uint32_t /*dstIndex*/)
{
	rendertarget::NotYet();
}

template <typename DerivedType>
void RenderTarget<DerivedType, kD3D12>::Blit(
	CommandBufferHandle<kD3D12> /*cmd*/,
	const IRenderTarget<kD3D12>& /*srcRenderTarget*/,
	const ImageSubresourceLayers<kD3D12>& /*srcSubresource*/,
	uint32_t /*srcIndex*/,
	const ImageSubresourceLayers<kD3D12>& /*dstSubresource*/,
	uint32_t /*dstIndex*/,
	Filter /*filter*/)
{
	rendertarget::NotYet();
}

template <typename DerivedType>
void RenderTarget<DerivedType, kD3D12>::SetLoadOp(LoadOp loadOp, uint32_t index, LoadOp stencilLoadOp)
{
	myOps.at(index).load = loadOp;
	myOps.at(index).stencilLoad = stencilLoadOp;
}

template <typename DerivedType>
void RenderTarget<DerivedType, kD3D12>::SetStoreOp(StoreOp storeOp, uint32_t index, StoreOp stencilStoreOp)
{
	myOps.at(index).store = storeOp;
	myOps.at(index).stencilStore = stencilStoreOp;
}

template <typename DerivedType>
void RenderTarget<DerivedType, kD3D12>::SetClearValue(const ClearValue& /*value*/, uint32_t /*index*/)
{
	rendertarget::NotYet();
}

template class RenderTarget<RenderImageSet<kD3D12>, kD3D12>;
template class RenderTarget<Frame<kD3D12>, kD3D12>;

} // namespace rhi
