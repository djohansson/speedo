// the D3D12 backend's render targets (rhi/d3d12/rendertarget.inl): their attachments' render target and depth stencil
// views (the target's own, in the device's cpu heaps), set on the command list at each Begin with the clears the loads
// ask for (D3D12 clears a view: ClearRenderTargetView, ClearDepthStencilView, and of rects too, which Metal's backend
// draws), copies between targets' images, and scaled blits: a draw (D3D12 has no blits), a triangle over the destination
// sampling the source where Vulkan's blit does, of a small shader pair compiled at the first (d3dcompiler, Windows'
// own). instantiated here for the backend's render targets (RenderImageSet, and the swapchain's frames)
#include <rhi/d3d12/backend.h>

#include <rhi/frame.h>
#include <rhi/renderimageset.h>
#include <rhi/rhi.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

namespace rhi
{

namespace rendertarget
{

// an attachment's view: its render target's or depth stencil's descriptor (of the attachment's format, its first level),
// and the shader's views its image's usage allows (gfx samples and writes attachments' views, as Vulkan's)
static d3d12::ImageViewObject* NewView(d3d12::AllocatorObject& allocator, d3d12::ImageObject* image, Format format)
{
	const bool color = HasColorComponent(format);
	ENSUREF(
		(image->flags & (color ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0,
		"{}: an attachment of an image without {} attachment usage",
		d3d12::GetName(image->resource),
		color ? "color" : "depth stencil");
	return d3d12::NewImageView(
		allocator, image, {.format = format, .aspectFlags = color ? ImageAspect::kColor : ImageAspect::kDepth, .levelCount = 1});
}

// an attachment cleared (all of it, or rects), as its format has it: color, or depth and stencil
static void ClearView(
	d3d12::CommandBufferObject& cmd, const d3d12::ImageViewObject& view, Format format, const ClearValue& value, bool depth, bool stencil, std::span<const D3D12_RECT> rects)
{
	if (view.renderTarget.ptr != 0)
	{
		cmd.list->ClearRenderTargetView(view.renderTarget, value.color.data(), static_cast<UINT>(rects.size()), rects.empty() ? nullptr : rects.data());
		return;
	}
	auto flags = static_cast<D3D12_CLEAR_FLAGS>(0);
	if (depth && HasDepthComponent(format))
		flags |= D3D12_CLEAR_FLAG_DEPTH;
	if (stencil && HasStencilComponent(format))
		flags |= D3D12_CLEAR_FLAG_STENCIL;
	if (flags != 0)
		cmd.list->ClearDepthStencilView(
			view.depthStencil, flags, value.depth, static_cast<UINT8>(value.stencil), static_cast<UINT>(rects.size()), rects.empty() ? nullptr : rects.data());
}

// the scaled blits' shaders (HLSL, d3dcompiler's shader model 5.0: D3D12 takes its bytecode): a triangle over the
// destination, each pixel the source's texel where Vulkan's nearest blit takes it (floor((x + 0.5) × scale): read, not
// sampled, so that a coordinate on a texel's edge rounds as Vulkan's does), or sampled at the pixel's center for a linear
// one. Metal's backend draws its scaled blits the same way (rhi/metal/rendertarget.cpp)
constexpr std::string_view kBlitSource = R"(
cbuffer BlitConstants : register(b0)
{
	float2 scale; // the source's size over the destination's
	float2 destinationTexel; // 1 over the destination's size
	uint nearest;
};
Texture2D<float4> gSource : register(t0);
SamplerState gLinear : register(s0);

float4 BlitVertex(uint id : SV_VertexID) : SV_Position
{
	const float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 BlitPixel(float4 position : SV_Position) : SV_Target
{
	if (nearest != 0)
		return gSource.Load(int3(floor(position.xy * scale), 0));
	return gSource.SampleLevel(gLinear, position.xy * destinationTexel, 0.0);
}
)";

static std::vector<char> CompileBlitShader(const char* entryPoint, const char* target)
{
	ID3DBlob* code = nullptr;
	ID3DBlob* errors = nullptr;
	const HRESULT result =
		D3DCompile(kBlitSource.data(), kBlitSource.size(), "blit", nullptr, nullptr, entryPoint, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
	ENSUREF(
		SUCCEEDED(result),
		"the scaled blits' {} failed to compile ({:#010x}): {}",
		entryPoint,
		static_cast<uint32_t>(result),
		errors != nullptr ? std::string_view(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "");
	d3d12::Release(errors);
	std::vector<char> bytes(static_cast<const char*>(code->GetBufferPointer()), static_cast<const char*>(code->GetBufferPointer()) + code->GetBufferSize());
	d3d12::Release(code);
	return bytes;
}

// the scaled blits' pipeline state for a destination format, its root signature and shaders made at the first (the
// constants, the source's table, a linear sampler clamped to its edge)
static std::pair<ID3D12RootSignature*, ID3D12PipelineState*> BlitPipeline(d3d12::AllocatorObject& allocator, DXGI_FORMAT format)
{
	auto& blits = allocator.blits;
	std::scoped_lock lock{blits.mutex};
	if (blits.rootSignature == nullptr)
	{
		const D3D12_DESCRIPTOR_RANGE1 range{
			.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
			.NumDescriptors = 1,
			.BaseShaderRegister = 0,
			.RegisterSpace = 0,
			.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE,
			.OffsetInDescriptorsFromTableStart = 0};
		const std::array parameters{
			D3D12_ROOT_PARAMETER1{
				.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
				.Constants = {.ShaderRegister = 0, .RegisterSpace = 0, .Num32BitValues = 5},
				.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL},
			D3D12_ROOT_PARAMETER1{
				.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
				.DescriptorTable = {.NumDescriptorRanges = 1, .pDescriptorRanges = &range},
				.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL},
		};
		const D3D12_STATIC_SAMPLER_DESC sampler{
			.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR,
			.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
			.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
			.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
			.MaxAnisotropy = 1,
			.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER,
			.MaxLOD = D3D12_FLOAT32_MAX,
			.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL};
		const D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{
			.Version = D3D_ROOT_SIGNATURE_VERSION_1_1,
			.Desc_1_1 = {
				.NumParameters = static_cast<UINT>(parameters.size()),
				.pParameters = parameters.data(),
				.NumStaticSamplers = 1,
				.pStaticSamplers = &sampler,
				.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE}};
		ID3DBlob* blob = nullptr;
		d3d12::Check(D3D12SerializeVersionedRootSignature(&desc, &blob, nullptr), "D3D12SerializeVersionedRootSignature (the blits')");
		d3d12::Check(
			allocator.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&blits.rootSignature)),
			"CreateRootSignature (the blits')");
		d3d12::Release(blob);
		blits.vertexShader = CompileBlitShader("BlitVertex", "vs_5_0");
		blits.pixelShader = CompileBlitShader("BlitPixel", "ps_5_0");
	}
	if (auto it = std::ranges::find(blits.pipelines, format, &std::pair<DXGI_FORMAT, ID3D12PipelineState*>::first); it != blits.pipelines.end())
		return {blits.rootSignature, it->second};
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
	desc.pRootSignature = blits.rootSignature;
	desc.VS = {.pShaderBytecode = blits.vertexShader.data(), .BytecodeLength = blits.vertexShader.size()};
	desc.PS = {.pShaderBytecode = blits.pixelShader.data(), .BytecodeLength = blits.pixelShader.size()};
	desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	desc.SampleMask = UINT_MAX;
	desc.RasterizerState = D3D12_RASTERIZER_DESC{.FillMode = D3D12_FILL_MODE_SOLID, .CullMode = D3D12_CULL_MODE_NONE, .DepthClipEnable = TRUE};
	desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.NumRenderTargets = 1;
	desc.RTVFormats[0] = format;
	desc.SampleDesc = {.Count = 1, .Quality = 0};
	ID3D12PipelineState* pipeline = nullptr;
	d3d12::Check(allocator.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)), "CreateGraphicsPipelineState (a blit's)");
	blits.pipelines.emplace_back(format, pipeline);
	return {blits.rootSignature, pipeline};
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
	if (myAttachments.empty())
		return;
	auto& allocator = d3d12::AllocatorObject::ForDevice(this->GetDesc().device);
	for (auto* view : myAttachments)
		d3d12::DeleteImageView(allocator, view);
	myAttachments.clear();
}

template <typename DerivedType>
void RenderTarget<DerivedType, kD3D12>::InternalUpdateAttachments()
{
	// an attachment's views are its image's, whatever its layout or aspect: made again only where the images are others
	// (a set that holds one, as gfx's sets hold an output's, holds copies of its descriptors)
	const auto& images = this->GetDesc().images;
	if (std::ranges::equal(myAttachments, images, {}, [](const auto* view) { return view->image; }))
		return;
	InternalReleaseAttachments();
	auto& allocator = d3d12::AllocatorObject::ForDevice(this->GetDesc().device);
	for (size_t imageIt = 0; imageIt < images.size(); imageIt++)
		myAttachments.push_back(rendertarget::NewView(allocator, images[imageIt], this->GetDesc().imageFormats[imageIt]));
}

template <typename DerivedType>
const RenderTargetBeginInfo<kD3D12>& RenderTarget<DerivedType, kD3D12>::Begin(CommandBufferHandle<kD3D12> cmd, SubpassContents /*contents*/)
{
	ZoneScopedN("RenderTarget::Begin");

	// the attachments set, and cleared where their loads ask (the attachments are in their layouts: a dynamic rendering
	// pass transitions nothing, as on Vulkan); stores need nothing (a don't care is a store)
	myBeginInfo = DynamicRenderingInfo<kD3D12>{.renderInfo = {.primary = cmd}, .inheritanceInfo = {}};
	std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kMaxColorAttachments> colors{};
	UINT colorCount = 0;
	const D3D12_CPU_DESCRIPTOR_HANDLE* depthStencil = nullptr;
	for (const auto* view : myAttachments)
	{
		if (view->renderTarget.ptr != 0)
			colors.at(colorCount++) = view->renderTarget;
		else
			depthStencil = &view->depthStencil;
	}
	cmd->list->OMSetRenderTargets(colorCount, colors.data(), FALSE, depthStencil);
	const auto& desc = this->GetDesc();
	for (size_t attachmentIt = 0; attachmentIt < myAttachments.size(); attachmentIt++)
	{
		const auto& ops = myOps[attachmentIt];
		const bool depth = ops.load == LoadOp::kClear;
		const bool stencil = ops.stencilLoad == LoadOp::kClear;
		if (depth || stencil)
			rendertarget::ClearView(*cmd, *myAttachments[attachmentIt], desc.imageFormats[attachmentIt], desc.clearValues[attachmentIt], depth, stencil, {});
	}
	return myBeginInfo;
}

template <typename DerivedType>
void RenderTarget<DerivedType, kD3D12>::End(CommandBufferHandle<kD3D12> /*cmd*/)
{
	// nothing to end: the attachments stay set until the next Begin, and need no store
}

template <typename DerivedType>
void RenderTarget<DerivedType, kD3D12>::ClearAll(CommandBufferHandle<kD3D12> cmd, std::span<const ClearValue> values) const
{
	ZoneScopedN("RenderTarget::ClearAll");

	const auto& desc = this->GetDesc();
	for (size_t attachmentIt = 0; attachmentIt < myAttachments.size(); attachmentIt++)
		rendertarget::ClearView(
			*cmd,
			*myAttachments[attachmentIt],
			desc.imageFormats[attachmentIt],
			attachmentIt < values.size() ? values[attachmentIt] : desc.clearValues[attachmentIt],
			true,
			true,
			{});
}

template <typename DerivedType>
void RenderTarget<DerivedType, kD3D12>::Clear(CommandBufferHandle<kD3D12> cmd, const ClearValue& value, uint32_t index)
{
	ZoneScopedN("RenderTarget::Clear");

	ENSURE(index < myAttachments.size());
	rendertarget::ClearView(*cmd, *myAttachments[index], this->GetDesc().imageFormats[index], value, true, true, {});
}

template <typename DerivedType>
void RenderTarget<DerivedType, kD3D12>::ClearRect(CommandBufferHandle<kD3D12> cmd, const ClearValue& value, uint32_t index, const Rect& rect) const
{
	ZoneScopedN("RenderTarget::ClearRect");

	ENSURE(index < myAttachments.size());
	const D3D12_RECT d3dRect{
		.left = rect.x, .top = rect.y, .right = rect.x + static_cast<LONG>(rect.width), .bottom = rect.y + static_cast<LONG>(rect.height)};
	rendertarget::ClearView(*cmd, *myAttachments[index], this->GetDesc().imageFormats[index], value, true, true, std::span(&d3dRect, 1));
}

template <typename DerivedType>
void RenderTarget<DerivedType, kD3D12>::Copy(
	CommandBufferHandle<kD3D12> cmd,
	const IRenderTarget<kD3D12>& srcRenderTarget,
	const ImageSubresourceLayers<kD3D12>& /*srcSubresource*/,
	uint32_t srcIndex,
	const ImageSubresourceLayers<kD3D12>& /*dstSubresource*/,
	uint32_t dstIndex)
{
	ZoneScopedN("RenderTarget::Copy");

	// the whole of each (their first level and layer: an attachment's), in their copies' layouts (the caller's, as Vulkan's)
	const D3D12_TEXTURE_COPY_LOCATION source{
		.pResource = srcRenderTarget.GetImages()[srcIndex]->resource, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0};
	const D3D12_TEXTURE_COPY_LOCATION destination{
		.pResource = this->GetDesc().images[dstIndex]->resource, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0};
	cmd->list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
}

template <typename DerivedType>
void RenderTarget<DerivedType, kD3D12>::Blit(
	CommandBufferHandle<kD3D12> cmd,
	const IRenderTarget<kD3D12>& srcRenderTarget,
	const ImageSubresourceLayers<kD3D12>& srcSubresource,
	uint32_t srcIndex,
	const ImageSubresourceLayers<kD3D12>& dstSubresource,
	uint32_t dstIndex,
	Filter filter)
{
	ZoneScopedN("RenderTarget::Blit");

	// the source's whole into the destination's whole (their first level and layer: an attachment's), as Vulkan's blit
	// leaves them: the destination a transfer's destination, the source in its layout. of the same size and format a
	// copy; else a draw over the destination sampling the source (shader resource while it's read)
	const auto srcExtent = srcRenderTarget.GetExtent();
	const auto dstExtent = GetExtent();
	auto* source = srcRenderTarget.GetImages()[srcIndex];
	auto* destination = this->GetDesc().images[dstIndex];
	this->Transition(cmd, ImageLayout::kTransferDestination, ImageAspect::kColor, dstIndex);
	if (srcExtent.width == dstExtent.width && srcExtent.height == dstExtent.height && source->format == destination->format)
	{
		Copy(cmd, srcRenderTarget, srcSubresource, srcIndex, dstSubresource, dstIndex);
		return;
	}
	const auto format = this->GetDesc().imageFormats[dstIndex];
	ENSUREF(
		HasColorComponent(format) && format != Format::kR32G32Uint,
		"the D3D12 backend's scaled blits write color attachments of floats alone (a draw), not attachment {}'s format {}",
		dstIndex,
		std::to_underlying(format));
	ENSUREF(
		(source->flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0,
		"a scaled blit's source (attachment {}) can't be sampled (a depth image without sampled usage)",
		srcIndex);

	auto& allocator = *cmd->device;
	const auto [rootSignature, pipeline] = rendertarget::BlitPipeline(allocator, d3d12::ToD3D12(format));
	// the source's view, in a range the command buffer takes (freed when its commands are done)
	const uint32_t sourceView = allocator.pool.resources.Allocate(1);
	cmd->pushedResources.emplace_back(sourceView, 1);
	const D3D12_SHADER_RESOURCE_VIEW_DESC srv{
		.Format = d3d12::ToD3D12(source->format),
		.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
		.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
		.Texture2D = {.MostDetailedMip = 0, .MipLevels = 1, .PlaneSlice = 0, .ResourceMinLODClamp = 0.0F}};
	allocator.device->CreateShaderResourceView(source->resource, &srv, allocator.pool.resources.Cpu(sourceView));

	const auto sourceLayout = source->layout;
	const auto destinationLayout = destination->layout;
	d3d12::TransitionImage(*cmd, *source, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);
	d3d12::TransitionImage(*cmd, *destination, D3D12_BARRIER_LAYOUT_RENDER_TARGET);
	// all of it written: its contents discarded, which also initializes it where it's new (a resource not zeroed must be,
	// in an attachment's layout: the debug layer's error 1422)
	cmd->list->DiscardResource(destination->resource, nullptr);
	const struct
	{
		std::array<float, 2> scale;
		std::array<float, 2> destinationTexel;
		uint32_t nearest;
	} constants{
		.scale = {static_cast<float>(srcExtent.width) / static_cast<float>(dstExtent.width),
				  static_cast<float>(srcExtent.height) / static_cast<float>(dstExtent.height)},
		.destinationTexel = {1.0F / static_cast<float>(dstExtent.width), 1.0F / static_cast<float>(dstExtent.height)},
		.nearest = filter == Filter::kNearest ? 1U : 0U};
	const D3D12_VIEWPORT viewport{
		.TopLeftX = 0.0F, .TopLeftY = 0.0F, .Width = static_cast<float>(dstExtent.width), .Height = static_cast<float>(dstExtent.height), .MinDepth = 0.0F, .MaxDepth = 1.0F};
	const D3D12_RECT scissor{.left = 0, .top = 0, .right = static_cast<LONG>(dstExtent.width), .bottom = static_cast<LONG>(dstExtent.height)};
	cmd->list->OMSetRenderTargets(1, &myAttachments[dstIndex]->renderTarget, FALSE, nullptr);
	cmd->list->SetGraphicsRootSignature(rootSignature);
	cmd->graphicsRootSignature = rootSignature; // (the next draw's PrepareDraw sets its own again)
	cmd->list->SetPipelineState(pipeline);
	cmd->list->SetGraphicsRoot32BitConstants(0, 5, &constants, 0);
	cmd->list->SetGraphicsRootDescriptorTable(1, allocator.pool.resources.Gpu(sourceView));
	cmd->list->RSSetViewports(1, &viewport);
	cmd->list->RSSetScissorRects(1, &scissor);
	cmd->list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cmd->list->DrawInstanced(3, 1, 0, 0);
	d3d12::TransitionImage(*cmd, *destination, destinationLayout);
	d3d12::TransitionImage(*cmd, *source, sourceLayout);
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
void RenderTarget<DerivedType, kD3D12>::SetClearValue(const ClearValue& value, uint32_t index)
{
	ENSURE(index < this->GetDesc().clearValues.size());
	this->InternalGetDesc().clearValues[index] = value;
}

// before the render targets' instantiation, which takes the device
IMPLEMENT_OBJECT_GETINSTANCE(RenderImageSet<kD3D12>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(RenderImageSet<kD3D12>);

template class RenderTarget<RenderImageSet<kD3D12>, kD3D12>;
template class RenderTarget<Frame<kD3D12>, kD3D12>;

template <>
void RenderImageSet<kD3D12>::End(CommandBufferHandle<kD3D12> cmd)
{
	SuperType::End(cmd);
	for (uint32_t imageIt = 0; imageIt < GetImageCount(); imageIt++)
	{
		auto& image = *myImages[imageIt];
		image.InternalSetImageLayout(
			HasColorComponent(image.GetDesc().format) ? ImageLayout::kColorAttachment : ImageLayout::kDepthStencilAttachment);
	}
}

} // namespace rhi
