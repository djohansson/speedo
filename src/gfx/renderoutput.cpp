#include "renderoutput.h"
#include "renderer.h"

#include <core/assert.h>

#include <format>

namespace gfx
{

using namespace rhi;

SwapchainOutput::SwapchainOutput(RHI& rhi, WindowHandle window)
	: myRHI(rhi)
	, myWindow(window)
{}

Swapchain& SwapchainOutput::InternalSwapchain() const
{
	return myRHI.GetSwapchain(myWindow);
}

Extent2d SwapchainOutput::GetExtent() const
{
	return InternalSwapchain().GetDesc().extent;
}

uint32_t SwapchainOutput::GetImageCount() const
{
	return static_cast<uint32_t>(InternalSwapchain().GetFrames().size());
}

ImageViewHandle SwapchainOutput::GetImageView(uint32_t imageIndex) const
{
	return InternalSwapchain().GetFrames()[imageIndex].GetAttachments()[0];
}

IRenderTarget& SwapchainOutput::GetTarget(uint32_t /*imageIndex*/)
{
	// the swapchain itself: its current (acquired) image
	return InternalSwapchain();
}

void SwapchainOutput::Prepare(CommandBufferHandle /*cmd*/)
{
	// no layout transitions for the swapchain images here: they may only be used once acquired. the frame graph
	// transitions each acquired image from its tracked layout (UNDEFINED for a new swapchain)
	for (auto& frame : InternalSwapchain().GetFrames())
	{
		frame.SetLoadOp(LoadOp::kClear, 0);
		frame.SetStoreOp(StoreOp::kStore, 0);
	}
}

void SwapchainOutput::AddFinalPasses(FrameGraph& graph, FrameGraph::ImageId output, const RecordingFrame& frame)
{
	// the ui: its texture uploads, then its draws over the frame, which is then presented
	graph.AddPass(
		"UI Uploads",
		[](FrameGraph::PassBuilder& builder) { builder.SideEffects(); },
		[&frame](FrameGraph::PassContext& context)
		{
			frame.inputs->prepareUi(context.cmd);
		});
	graph.AddPass(
		"UI",
		[output, &frame](FrameGraph::PassBuilder& builder)
		{
			builder.ColorAttachment(output, LoadOp::kLoad);
		},
		[this, &frame](FrameGraph::PassContext& context)
		{
			// the ui's pipelines are made for the swapchain's frames
			frame.pipeline->SetRenderTarget(InternalSwapchain().GetFrames()[frame.inputs->outputIndex]);
			frame.inputs->drawUi(context.cmd);
		});
	graph.AddPass(
		"Present",
		[output](FrameGraph::PassBuilder& builder)
		{
			builder.Use(output, ResourceUse::Present());
			builder.SideEffects();
		},
		[](FrameGraph::PassContext& /*context*/) {});
}

ImageOutput::ImageOutput(Device& device, Extent2d extent)
	: myDevice(device)
{
	Resize(extent);
}

void ImageOutput::Resize(Extent2d extent)
{
	myExtent = extent;
	myTarget.reset();
	myImage = std::make_shared<Image>(ImageCreateDesc{
		myDevice.CreateDeviceObjectCreateDesc("Image Output"),
		{{.extent = extent}},
		Format::kR8G8B8A8Unorm,
		ImageTiling::kOptimal,
		ImageUsage::kStorage | ImageUsage::kSampled | ImageUsage::kColorAttachment,
		MemoryProperty::kDeviceLocal,
		ImageAspect::kColor,
		ImageLayout::kUndefined});
	myTarget = std::make_unique<RenderImageSet>(std::vector{myImage});
}

ImageViewHandle ImageOutput::GetView() const
{
	return myTarget->GetAttachments()[0];
}

uint32_t ImageOutput::GetImageCount() const
{
	return 1;
}

ImageViewHandle ImageOutput::GetImageView(uint32_t /*imageIndex*/) const
{
	return GetView();
}

IRenderTarget& ImageOutput::GetTarget(uint32_t /*imageIndex*/)
{
	return *myTarget;
}

void ImageOutput::Prepare(CommandBufferHandle cmd)
{
	// the ui may show it before its first frame is drawn
	myTarget->Transition(cmd, ImageLayout::kShaderReadOnly, ImageAspect::kColor, 0);
}

void ImageOutput::AddFinalPasses(FrameGraph& graph, FrameGraph::ImageId output, const RecordingFrame& /*frame*/)
{
	// the image, for sampling (by the ui) once the frame is done
	graph.AddPass(
		"Output",
		[output](FrameGraph::PassBuilder& builder)
		{
			builder.Use(output, ResourceUse::Sampled(PipelineStage::kFragmentShader));
			builder.SideEffects();
		},
		[](FrameGraph::PassContext& /*context*/) {});
}

} // namespace gfx
