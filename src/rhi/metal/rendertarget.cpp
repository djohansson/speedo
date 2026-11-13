// the Metal backend's render targets (rhi/metal/rendertarget.inl): a render pass descriptor at each Begin (each
// attachment's texture, load and store actions and clear value), its render encoder open until End; clears are passes
// of their own (cleared, stored, ended at once), a clear of a rect inside a pass a draw over the rect (Metal clears no
// part of an attachment); a copy is the blit encoder's. A scaled blit needs a draw too (later)
#include <rhi/metal/backend.h>

#include <rhi/frame.h>
#include <rhi/renderimageset.h>
#include <rhi/rhi.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <algorithm>
#include <format>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace rhi
{

namespace rendertarget
{

static MTL::LoadAction ToMetal(LoadOp op) noexcept
{
	switch (op)
	{
	case LoadOp::kLoad: return MTL::LoadActionLoad;
	case LoadOp::kClear: return MTL::LoadActionClear;
	case LoadOp::kDontCare: return MTL::LoadActionDontCare;
	}
	return MTL::LoadActionDontCare;
}

static MTL::StoreAction ToMetal(StoreOp op) noexcept
{
	return op == StoreOp::kStore ? MTL::StoreActionStore : MTL::StoreActionDontCare;
}

// an attachment of a render pass descriptor: its texture, what the pass does with it, and its clear value
struct PassAttachment
{
	MTL::Texture* texture = nullptr;
	Format format{};
	LoadOp load = LoadOp::kDontCare;
	StoreOp store = StoreOp::kDontCare;
	LoadOp stencilLoad = LoadOp::kDontCare;
	StoreOp stencilStore = StoreOp::kDontCare;
	ClearValue clear{};
};

// a render pass over attachments, begun on cmd (its encoder made at its first command): color attachments in their
// order, a depth stencil one's depth and stencil
static void BeginPass(metal::CommandBufferObject& cmd, std::span<const PassAttachment> attachments)
{
	auto* descriptor = MTL::RenderPassDescriptor::alloc()->init();
	NS::UInteger colorIt = 0;
	for (const auto& attachment : attachments)
	{
		if (HasColorComponent(attachment.format))
		{
			auto* color = descriptor->colorAttachments()->object(colorIt++);
			color->setTexture(attachment.texture);
			color->setLoadAction(ToMetal(attachment.load));
			color->setStoreAction(ToMetal(attachment.store));
			const auto& value = attachment.clear.color;
			color->setClearColor(MTL::ClearColor(value[0], value[1], value[2], value[3]));
			continue;
		}
		if (HasDepthComponent(attachment.format))
		{
			auto* depth = descriptor->depthAttachment();
			depth->setTexture(attachment.texture);
			depth->setLoadAction(ToMetal(attachment.load));
			depth->setStoreAction(ToMetal(attachment.store));
			depth->setClearDepth(attachment.clear.depth);
		}
		if (HasStencilComponent(attachment.format))
		{
			auto* stencil = descriptor->stencilAttachment();
			stencil->setTexture(attachment.texture);
			stencil->setLoadAction(ToMetal(attachment.stencilLoad));
			stencil->setStoreAction(ToMetal(attachment.stencilStore));
			stencil->setClearStencil(attachment.clear.stencil);
		}
	}
	cmd.BeginRenderPass(descriptor);
	descriptor->release();
}

// ClearRect's shaders: a triangle over the viewport (set to the rect) at the clear's depth, and per color attachment
// slot a fragment function writing the clear value's bits as the attachment's components take them, floats or unsigned
// integers (Vulkan's clear color is a union of the same bytes: rhi's ClearValue's floats, an integer attachment's bits).
// [[color(n)]] takes a constant, so a function each slot (kMaxColorAttachments of them)
static_assert(kMaxColorAttachments == 4, "ClearRect's shaders have a fragment function each color attachment slot");
constexpr const char* kClearSource = R"(
#include <metal_stdlib>
using namespace metal;

struct ClearVertexOut
{
	float4 position [[position]];
};

vertex ClearVertexOut ClearVertex(uint vertexId [[vertex_id]], constant float& depth [[buffer(0)]])
{
	const float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
	return {float4(uv * 2.0 - 1.0, depth, 1.0)};
}

#define CLEAR_FRAGMENT(slot, type, suffix) \
	struct ClearOut##slot##suffix { type value [[color(slot)]]; }; \
	fragment ClearOut##slot##suffix ClearFragment##slot##suffix(constant type& value [[buffer(0)]]) { return {value}; }
#define CLEAR_FRAGMENTS(slot) CLEAR_FRAGMENT(slot, float4, Float) CLEAR_FRAGMENT(slot, uint4, Uint)

CLEAR_FRAGMENTS(0)
CLEAR_FRAGMENTS(1)
CLEAR_FRAGMENTS(2)
CLEAR_FRAGMENTS(3)
)";

static MTL::Function* NewFunction(MTL::Library* library, const std::string& name)
{
	MTL::Function* function = library->newFunction(NS::String::string(name.c_str(), NS::UTF8StringEncoding));
	ENSUREF(function != nullptr, "ClearRect's shaders have no function {}", name);
	return function;
}

static MTL::DepthStencilState* NewDepthStencilState(MTL::Device* device, bool write)
{
	auto* descriptor = MTL::DepthStencilDescriptor::alloc()->init();
	descriptor->setDepthCompareFunction(MTL::CompareFunctionAlways);
	descriptor->setDepthWriteEnabled(write);
	if (write)
	{
		auto* stencil = MTL::StencilDescriptor::alloc()->init();
		stencil->setStencilCompareFunction(MTL::CompareFunctionAlways);
		stencil->setDepthStencilPassOperation(MTL::StencilOperationReplace);
		stencil->setReadMask(0xFFU);
		stencil->setWriteMask(0xFFU);
		descriptor->setFrontFaceStencil(stencil);
		descriptor->setBackFaceStencil(stencil);
		stencil->release();
	}
	auto* state = device->newDepthStencilState(descriptor);
	descriptor->release();
	ENSUREF(state != nullptr, "Failed to make ClearRect's depth stencil state");
	return state;
}

// clear's pipeline for the attachment at index of a target of formats (its pass's: Metal's pipelines name every
// attachment's format), made at its first clear: the library and depth stencil states with the target's first
static MTL::RenderPipelineState* ClearPipeline(
	metal::RenderTargetClearObject& clear, MTL::Device* device, std::span<const Format> formats, uint32_t index)
{
	std::scoped_lock lock{clear.mutex};
	if (clear.pipelines[index] != nullptr)
		return clear.pipelines[index];

	if (clear.library == nullptr)
	{
		ZoneScopedN("RenderTarget::ClearRect::newLibraryWithSource");

		auto* source = NS::String::alloc()->init(kClearSource, NS::UTF8StringEncoding);
		auto* options = MTL::CompileOptions::alloc()->init();
		options->setLanguageVersion(MTL::LanguageVersion3_2);
		NS::Error* error = nullptr;
		clear.library = device->newLibrary(source, options, &error);
		options->release();
		source->release();
		ENSUREF(
			clear.library != nullptr,
			"Failed to compile ClearRect's MSL: {}",
			error != nullptr ? error->localizedDescription()->utf8String() : "(no error)");
		clear.keep = NewDepthStencilState(device, false);
		clear.write = NewDepthStencilState(device, true);
	}

	auto* descriptor = MTL::RenderPipelineDescriptor::alloc()->init();
	MTL::Function* vertex = NewFunction(clear.library, "ClearVertex");
	MTL::Function* fragment = nullptr;
	descriptor->setVertexFunction(vertex);
	NS::UInteger colorIt = 0;
	for (uint32_t formatIt = 0; formatIt < formats.size(); formatIt++)
	{
		const auto format = formats[formatIt];
		if (HasColorComponent(format))
		{
			auto* color = descriptor->colorAttachments()->object(colorIt);
			color->setPixelFormat(metal::ToMetal(format));
			color->setWriteMask(formatIt == index ? MTL::ColorWriteMaskAll : MTL::ColorWriteMaskNone);
			if (formatIt == index)
				fragment = NewFunction(
					clear.library,
					std::format("ClearFragment{}{}", colorIt, format == Format::kR32G32Uint ? "Uint" : "Float"));
			colorIt++;
			continue;
		}
		if (HasDepthComponent(format))
			descriptor->setDepthAttachmentPixelFormat(metal::ToMetal(format));
		if (HasStencilComponent(format))
			descriptor->setStencilAttachmentPixelFormat(metal::ToMetal(format));
	}
	// a depth stencil attachment's clear draws no color: no fragment stage
	descriptor->setFragmentFunction(fragment);
	NS::Error* error = nullptr;
	clear.pipelines[index] = device->newRenderPipelineState(descriptor, &error);
	descriptor->release();
	vertex->release();
	if (fragment != nullptr)
		fragment->release();
	ENSUREF(
		clear.pipelines[index] != nullptr,
		"Failed to make ClearRect's pipeline for attachment {}: {}",
		index,
		error != nullptr ? error->localizedDescription()->utf8String() : "(no error)");
	return clear.pipelines[index];
}

} // namespace rendertarget

namespace metal
{

RenderTargetClearObject::~RenderTargetClearObject()
{
	for (auto* pipeline : pipelines)
		if (pipeline != nullptr)
			pipeline->release();
	for (NS::Object* object : std::initializer_list<NS::Object*>{library, keep, write})
		if (object != nullptr)
			object->release();
}

} // namespace metal

template <typename DerivedType>
RenderTarget<DerivedType, kMetal>::RenderTarget(typename ObjectTraits<DerivedType>::CreateDescType&& desc)
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
	myClear = std::make_unique<metal::RenderTargetClearObject>();
	myClear->pipelines.resize(formats.size());
	InternalUpdateAttachments();
}

template <typename DerivedType>
RenderTarget<DerivedType, kMetal>::RenderTarget(RenderTarget&& other) noexcept
{
	Swap(other);
}

template <typename DerivedType>
RenderTarget<DerivedType, kMetal>::~RenderTarget()
{
	InternalReleaseAttachments();
}

template <typename DerivedType>
RenderTarget<DerivedType, kMetal>& RenderTarget<DerivedType, kMetal>::operator=(RenderTarget&& other) noexcept
{
	Swap(other);
	return *this;
}

template <typename DerivedType>
void RenderTarget<DerivedType, kMetal>::Swap(RenderTarget& rhs) noexcept
{
	DeviceObject<DerivedType>::Swap(rhs);
	std::swap(myAttachments, rhs.myAttachments);
	std::swap(myAttachmentDescs, rhs.myAttachmentDescs);
	std::swap(myOps, rhs.myOps);
	std::swap(myPipelineRenderingCreateInfo, rhs.myPipelineRenderingCreateInfo);
	std::swap(myBeginInfo, rhs.myBeginInfo);
	std::swap(myClear, rhs.myClear);
}

template <typename DerivedType>
void RenderTarget<DerivedType, kMetal>::InternalReleaseAttachments() noexcept
{
	for (auto* attachment : myAttachments)
	{
		attachment->texture->release();
		delete attachment;
	}
	myAttachments.clear();
}

template <typename DerivedType>
void RenderTarget<DerivedType, kMetal>::InternalUpdateAttachments()
{
	// an attachment's view is its image's texture, whatever its layout or aspect: made again only where the images are
	// others. a transition (RenderImageSet::Transition) made them again first, which freed views the descriptor sets held
	// (a segfault encoding one), as Vulkan's backend avoids for its own (see its InternalUpdateAttachments)
	const auto& images = this->GetDesc().images;
	if (std::ranges::equal(myAttachments, images, {}, [](const auto* view) { return view->texture; }))
		return;
	InternalReleaseAttachments();
	for (auto* texture : images)
		myAttachments.push_back(new metal::ImageViewObject{.texture = texture->retain()});
}

template <typename DerivedType>
const RenderTargetBeginInfo<kMetal>& RenderTarget<DerivedType, kMetal>::Begin(CommandBufferHandle<kMetal> cmd, SubpassContents contents)
{
	ZoneScopedN("RenderTarget::Begin");

	// secondary command buffers record inline too, into the pass's encoder: the begin info hands them the command buffer
	// (see CommandPool::BeginSecondary)
	(void)contents;
	myBeginInfo = DynamicRenderingInfo<kMetal>{.renderInfo = {.primary = cmd}, .inheritanceInfo = {}};
	std::vector<rendertarget::PassAttachment> attachments;
	attachments.reserve(myAttachments.size());
	for (size_t attachmentIt = 0; attachmentIt < myAttachments.size(); attachmentIt++)
	{
		const auto& ops = myOps[attachmentIt];
		attachments.push_back({
			.texture = myAttachments[attachmentIt]->texture,
			.format = this->GetDesc().imageFormats[attachmentIt],
			.load = ops.load,
			.store = ops.store,
			.stencilLoad = ops.stencilLoad,
			.stencilStore = ops.stencilStore,
			.clear = this->GetDesc().clearValues[attachmentIt],
		});
	}
	rendertarget::BeginPass(*cmd, attachments);
	return myBeginInfo;
}

template <typename DerivedType>
void RenderTarget<DerivedType, kMetal>::End(CommandBufferHandle<kMetal> cmd)
{
	cmd->EndEncoders();
}

template <typename DerivedType>
void RenderTarget<DerivedType, kMetal>::ClearAll(CommandBufferHandle<kMetal> cmd, std::span<const ClearValue> values) const
{
	ZoneScopedN("RenderTarget::ClearAll");

	std::vector<rendertarget::PassAttachment> attachments;
	for (size_t attachmentIt = 0; attachmentIt < myAttachments.size(); attachmentIt++)
		attachments.push_back({
			.texture = myAttachments[attachmentIt]->texture,
			.format = this->GetDesc().imageFormats[attachmentIt],
			.load = LoadOp::kClear,
			.store = StoreOp::kStore,
			.stencilLoad = LoadOp::kClear,
			.stencilStore = StoreOp::kStore,
			.clear = attachmentIt < values.size() ? values[attachmentIt] : this->GetDesc().clearValues[attachmentIt],
		});
	rendertarget::BeginPass(*cmd, attachments);
	cmd->EndEncoders();
}

template <typename DerivedType>
void RenderTarget<DerivedType, kMetal>::Clear(CommandBufferHandle<kMetal> cmd, const ClearValue& value, uint32_t index)
{
	ZoneScopedN("RenderTarget::Clear");

	ENSURE(index < myAttachments.size());
	const std::array attachments{rendertarget::PassAttachment{
		.texture = myAttachments[index]->texture,
		.format = this->GetDesc().imageFormats[index],
		.load = LoadOp::kClear,
		.store = StoreOp::kStore,
		.stencilLoad = LoadOp::kClear,
		.stencilStore = StoreOp::kStore,
		.clear = value,
	}};
	rendertarget::BeginPass(*cmd, attachments);
	cmd->EndEncoders();
}

template <typename DerivedType>
void RenderTarget<DerivedType, kMetal>::ClearRect(
	CommandBufferHandle<kMetal> cmd, const ClearValue& value, uint32_t index, const Rect& rect) const
{
	ZoneScopedN("RenderTarget::ClearRect");

	const auto& desc = this->GetDesc();
	ENSURE(index < desc.imageFormats.size());
	ENSUREF(desc.layerCount == 1, "the Metal backend clears a rect of one layer alone ({} layers)", desc.layerCount);

	// the rect inside the attachments (Metal's scissor must be)
	const auto extent = GetExtent();
	const auto x0 = static_cast<uint32_t>(std::max(rect.x, 0));
	const auto y0 = static_cast<uint32_t>(std::max(rect.y, 0));
	const auto x1 = std::min<int64_t>(int64_t{rect.x} + rect.width, extent.width);
	const auto y1 = std::min<int64_t>(int64_t{rect.y} + rect.height, extent.height);
	if (x1 <= x0 || y1 <= y0)
		return;
	const MTL::ScissorRect scissor{
		.x = x0, .y = y0, .width = static_cast<NS::UInteger>(x1 - x0), .height = static_cast<NS::UInteger>(y1 - y0)};

	auto* device = static_cast<MTL::Device*>(this->GetDevice());
	auto* pipeline = rendertarget::ClearPipeline(*myClear, device, desc.imageFormats, index);
	const bool color = HasColorComponent(desc.imageFormats[index]);

	auto* encoder = cmd->RenderEncoder();
	encoder->setRenderPipelineState(pipeline);
	encoder->setDepthStencilState(color ? myClear->keep : myClear->write);
	encoder->setCullMode(MTL::CullModeNone);
	encoder->setViewport(MTL::Viewport{
		.originX = static_cast<double>(scissor.x),
		.originY = static_cast<double>(scissor.y),
		.width = static_cast<double>(scissor.width),
		.height = static_cast<double>(scissor.height),
		.znear = 0.0,
		.zfar = 1.0});
	encoder->setScissorRect(scissor);
	encoder->setVertexBytes(&value.depth, sizeof(value.depth), 0);
	if (color)
		encoder->setFragmentBytes(value.color.data(), sizeof(value.color), 0);
	else
		encoder->setStencilReferenceValue(value.stencil);
	encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger{0}, NS::UInteger{3});

	// the pass's state as it was (the next draw sets its pipeline, depth stencil state and buffers again: PrepareDraw)
	const MTL::ScissorRect whole{.x = 0, .y = 0, .width = extent.width, .height = extent.height};
	encoder->setViewport(cmd->viewport.value_or(MTL::Viewport{
		.originX = 0.0,
		.originY = 0.0,
		.width = static_cast<double>(extent.width),
		.height = static_cast<double>(extent.height),
		.znear = 0.0,
		.zfar = 1.0}));
	encoder->setScissorRect(cmd->scissor.value_or(whole));
	encoder->setCullMode(cmd->cullMode);
	encoder->setStencilReferenceValue(cmd->stencilReference);
}

template <typename DerivedType>
void RenderTarget<DerivedType, kMetal>::Copy(
	CommandBufferHandle<kMetal> cmd,
	const IRenderTarget<kMetal>& srcRenderTarget,
	const ImageSubresourceLayers<kMetal>& /*srcSubresource*/,
	uint32_t srcIndex,
	const ImageSubresourceLayers<kMetal>& /*dstSubresource*/,
	uint32_t dstIndex)
{
	ZoneScopedN("RenderTarget::Copy");

	// the whole of each (their first level and layer: an attachment's)
	cmd->BlitEncoder()->copyFromTexture(srcRenderTarget.GetImages()[srcIndex], this->GetDesc().images[dstIndex]);
}

template <typename DerivedType>
void RenderTarget<DerivedType, kMetal>::Blit(
	CommandBufferHandle<kMetal> cmd,
	const IRenderTarget<kMetal>& srcRenderTarget,
	const ImageSubresourceLayers<kMetal>& srcSubresource,
	uint32_t srcIndex,
	const ImageSubresourceLayers<kMetal>& dstSubresource,
	uint32_t dstIndex,
	Filter /*filter*/)
{
	// of the same size, a copy; scaled, a draw (later)
	const auto srcExtent = srcRenderTarget.GetExtent();
	const auto dstExtent = GetExtent();
	ENSUREF(
		srcExtent.width == dstExtent.width && srcExtent.height == dstExtent.height,
		"the Metal backend blits between render targets of the same size alone yet ({}x{} to {}x{})",
		srcExtent.width,
		srcExtent.height,
		dstExtent.width,
		dstExtent.height);
	Copy(cmd, srcRenderTarget, srcSubresource, srcIndex, dstSubresource, dstIndex);
}

template <typename DerivedType>
void RenderTarget<DerivedType, kMetal>::SetLoadOp(LoadOp loadOp, uint32_t index, LoadOp stencilLoadOp)
{
	myOps[index].load = loadOp;
	if (HasStencilComponent(this->GetDesc().imageFormats[index]))
		myOps[index].stencilLoad = stencilLoadOp;
}

template <typename DerivedType>
void RenderTarget<DerivedType, kMetal>::SetStoreOp(StoreOp storeOp, uint32_t index, StoreOp stencilStoreOp)
{
	myOps[index].store = storeOp;
	if (HasStencilComponent(this->GetDesc().imageFormats[index]))
		myOps[index].stencilStore = stencilStoreOp;
}

template <typename DerivedType>
void RenderTarget<DerivedType, kMetal>::SetClearValue(const ClearValue& value, uint32_t index)
{
	ENSURE(index < this->GetDesc().clearValues.size());
	this->InternalGetDesc().clearValues[index] = value;
}

// before the render targets' instantiation, which takes the device (ClearRect)
IMPLEMENT_OBJECT_GETINSTANCE(RenderImageSet<kMetal>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(RenderImageSet<kMetal>);

// the backend's render targets (a swapchain's frames are made with it, step 6)
template class RenderTarget<RenderImageSet<kMetal>, kMetal>;
template class RenderTarget<Frame<kMetal>, kMetal>;

// a render image set's End: the pass leaves its images as Vulkan's render pass does (an attachment's layout: rhi's
// bookkeeping, Metal has none)
template <>
void RenderImageSet<kMetal>::End(CommandBufferHandle<kMetal> cmd)
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
