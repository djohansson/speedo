#include "renderer.h"

#include <gfx/model.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <algorithm>
#include <atomic>
#include <numeric>

namespace gfx
{

using namespace rhi;

namespace renderer
{

// ComputeMain has 16x16 threads per group, each copying a 16x16 pixel bucket
constexpr uint32_t kComputePixelsPerGroup = 16U * 16U;

[[nodiscard]] uint64_t PixelCount(Extent2d extent)
{
	return std::max<uint64_t>(static_cast<uint64_t>(extent.width) * extent.height, 1);
}

[[nodiscard]] uint32_t OitNodeCapacity(Extent2d extent)
{
	return static_cast<uint32_t>(std::min<uint64_t>(PixelCount(extent) * SHADER_TYPES_OIT_NODES_PER_PIXEL, SHADER_TYPES_OIT_NONE - 1));
}

} // namespace renderer

Renderer::Renderer(Device& device)
{
	using namespace renderer;

	// linear: shading and blending happen in linear space, and ComputeMain applies the srgb curve when it copies the
	// result to the swapchain
	myColor = myGraph.CreateImage({.name = "Main Color", .format = Format::kR16G16B16A16Sfloat});
	myDepth = myGraph.CreateImage(
		{.name = "Main Depth",
		 .format = device.FindSupportedFormat(
			 std::array{Format::kD32SfloatS8Uint, Format::kD24UnormS8Uint}, ImageTiling::kOptimal, FormatFeature::kDepthStencilAttachment),
		 .aspect = ImageAspect::kDepth | ImageAspect::kStencil});
	// the opaque scene with mips, which transmissive materials refract (SHADER_TYPES_TRANSMISSION_TEXTURE)
	myTransmission = myGraph.CreateImage({.name = "Transmission", .format = Format::kR16G16B16A16Sfloat, .mipChain = true});
	// the order independent transparency's per pixel lists (see OitNode): their heads (a node index per pixel of the
	// render target), nodes and counter
	myOitHeads = myGraph.CreateBuffer(
		{.name = "OIT Heads", .sizeOf = [](Extent2d extent) { return PixelCount(extent) * sizeof(uint32_t); }});
	myOitNodes = myGraph.CreateBuffer(
		{.name = "OIT Nodes", .sizeOf = [](Extent2d extent) { return std::max<uint64_t>(OitNodeCapacity(extent), 1) * sizeof(OitNode); }});
	myOitCounter = myGraph.CreateBuffer({.name = "OIT Counter", .size = sizeof(uint32_t)});
	mySwapchain = myGraph.ImportImage("Swapchain");
	myExposureHistogram = myGraph.ImportBuffer("Exposure Histogram");

	constexpr auto kOitAccess = Access::kShaderRead | Access::kShaderWrite;
	auto useOitLists = [this](FrameGraph::PassBuilder& builder, PipelineStage stages, Access access)
	{
		builder.Use(myOitHeads, ResourceUse::Storage(stages, access));
		builder.Use(myOitNodes, ResourceUse::Storage(stages, stages == PipelineStage::kFragmentShader ? Access::kShaderWrite : access));
		builder.Use(myOitCounter, ResourceUse::Storage(stages, access));
	};

	// empty transparency lists, once the previous frame's ComputeMain has read them
	myGraph.AddPass(
		"OIT Clear",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myOitHeads, ResourceUse::TransferDestination());
			builder.Use(myOitCounter, ResourceUse::TransferDestination());
		},
		[this](FrameGraph::PassContext& context)
		{
			CommandEncoder encoder(context.cmd);
			encoder.FillBuffer(context.graph.GetBuffer(myOitHeads), 0, 0, SHADER_TYPES_OIT_NONE);
			encoder.FillBuffer(context.graph.GetBuffer(myOitCounter), 0, 0, 0);
		});

	// the main pass: the views, each in its viewport. alpha 0 where nothing opaque is drawn (opaque draws write alpha 1):
	// there ComputeMain draws the environment
	auto mainPass = [&](MainPassPhase phase)
	{
		auto load = phase == MainPassPhase::kOpaque ? LoadOp::kClear : LoadOp::kLoad;
		return myGraph.AddPass(
			phase == MainPassPhase::kOpaque ? "Main" : "Main Transmissive",
			[this, load, phase, &useOitLists](FrameGraph::PassBuilder& builder)
			{
				builder.ColorAttachment(myColor, load, StoreOp::kStore, ClearValue{.color = {0.0F, 0.0F, 0.0F, 0.0F}});
				builder.DepthAttachment(myDepth, load);
				useOitLists(builder, PipelineStage::kFragmentShader, kOitAccess);
				if (phase == MainPassPhase::kTransmissive)
					builder.Use(myTransmission, ResourceUse::Sampled(PipelineStage::kFragmentShader));
				builder.Contents(SubpassContents::kSecondaryCommandBuffers);
				// the profiling scope's timestamps outside the render target: begun for secondary command buffers, it can
				// hold nothing else
				builder.Wrap(
					[this](CommandBufferHandle cmd, const std::function<void()>& record)
					{
						GPU_SCOPE(cmd, *myInputs->graphicsQueue, draw);
						record();
					});
			},
			[this, phase](FrameGraph::PassContext& context) { InternalDrawMainPass(context, *myPipeline, *myInputs, phase); });
	};
	mainPass(MainPassPhase::kOpaque);

	// transmissive materials see the opaque scene behind them: copied, with mips for their roughness
	myTransmissionPass = myGraph.AddPass(
		"Transmission",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myColor, ResourceUse::TransferSource());
			builder.Use(myTransmission, ResourceUse::TransferDestination());
		},
		[this](FrameGraph::PassContext& context)
		{
			GPU_SCOPE(context.cmd, *myInputs->graphicsQueue, transmission);
			auto& transmission = context.graph.GetImage(myTransmission);
			transmission.BlitFrom(context.cmd, context.graph.GetImage(myColor));
			transmission.GenerateMips(context.cmd, ImageLayout::kShaderReadOnly);
		});
	myTransmissivePhase = mainPass(MainPassPhase::kTransmissive);

	// the frame's exposure histogram, emptied for ComputeMain to count into
	myGraph.AddPass(
		"Exposure Clear",
		[this](FrameGraph::PassBuilder& builder) { builder.Use(myExposureHistogram, ResourceUse::TransferDestination()); },
		[this](FrameGraph::PassContext& context)
		{
			constexpr auto kHistogramSize = SHADER_TYPES_EXPOSURE_BINS * sizeof(uint32_t);
			CommandEncoder(context.cmd).FillBuffer(
				context.graph.GetBuffer(myExposureHistogram), myInputs->frameIndex * kHistogramSize, kHistogramSize, 0);
		});

	// the backdrop, the transparency's resolve, exposure and tonemapping, into the swapchain image
	myGraph.AddPass(
		"ComputeMain",
		[this, &useOitLists](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myColor, ResourceUse::Sampled(PipelineStage::kComputeShader));
			useOitLists(builder, PipelineStage::kComputeShader, Access::kShaderRead);
			builder.Use(myExposureHistogram, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite));
			builder.Use(mySwapchain, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderWrite));
		},
		[this](FrameGraph::PassContext& context)
		{
			const auto& inputs = *myInputs;
			auto& pipeline = *myPipeline;
			auto cmd = context.cmd;
			GPU_SCOPE(cmd, *inputs.graphicsQueue, computeMain);

			pipeline.BindLayoutAuto(pipeline.GetDevice().GetPipelineLayoutHandle("Main"), PipelineBindPoint::kCompute);
			pipeline.SetDescriptorData(
				"gRWTextures",
				ImageBinding{.sampler = {}, .imageView = inputs.swapchain->GetAttachments()[0], .layout = inputs.swapchain->GetLayout(0)},
				DESCRIPTOR_SET_CATEGORY_GLOBAL_RW_TEXTURES,
				inputs.frameIndex);

			pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES);
			pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_GLOBAL_RW_TEXTURES);
			// the backdrop's: the views, the environment and its sampler; and the transparency lists
			pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);
			pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_GLOBAL_SAMPLERS);
			pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_VIEW);
			pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES);
			pipeline.BindPipelineAuto(cmd);

			auto pushConstants = inputs.pushConstants;
			pipeline.PushConstants(cmd, std::as_bytes(std::span(&pushConstants, 1)));

			// cover the whole swapchain image
			auto extent = inputs.swapchain->GetExtent();
			CommandEncoder(cmd).Dispatch(
				(extent.width + kComputePixelsPerGroup - 1) / kComputePixelsPerGroup,
				(extent.height + kComputePixelsPerGroup - 1) / kComputePixelsPerGroup,
				1U);
		});

	// for the host to read the histogram once the frame is done (see UpdateAutoExposure)
	myGraph.AddPass(
		"Exposure Readback",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myExposureHistogram, ResourceUse::HostRead());
			builder.SideEffects();
		},
		[](FrameGraph::PassContext& /*context*/) {});

	// the ui: its texture uploads, then its draws over the frame
	myGraph.AddPass(
		"UI Uploads",
		[](FrameGraph::PassBuilder& builder) { builder.SideEffects(); },
		[this](FrameGraph::PassContext& context)
		{
			GPU_SCOPE(context.cmd, *myInputs->graphicsQueue, imguiTextures);
			if (myInputs->prepareUi)
				myInputs->prepareUi(context.cmd);
		});
	myGraph.AddPass(
		"UI",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.ColorAttachment(mySwapchain, LoadOp::kLoad);
			builder.Wrap(
				[this](CommandBufferHandle cmd, const std::function<void()>& record)
				{
					GPU_SCOPE(cmd, *myInputs->graphicsQueue, imgui);
					record();
				});
		},
		[this](FrameGraph::PassContext& context)
		{
			myPipeline->SetRenderTarget(*myInputs->swapchainFrame);
			if (myInputs->drawUi)
				myInputs->drawUi(context.cmd);
		});

	myGraph.AddPass(
		"Present",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.Use(mySwapchain, ResourceUse::Present());
			builder.SideEffects();
		},
		[](FrameGraph::PassContext& /*context*/) {});
}

void Renderer::Resize(Device& device, Pipeline& pipeline, Extent2d extent)
{
	ZoneScopedN("Renderer::Resize");

	if (!myGraph.Compile(device, extent))
		return;

	myOitNodeCapacity = renderer::OitNodeCapacity(extent);

	// a dirty set is updated as a whole, so every frame's slot is written up front, with the layouts the passes leave
	// them in (so that later writes of the same are skipped as unchanged)
	pipeline.BindLayoutAuto(device.GetPipelineLayoutHandle("Main"), PipelineBindPoint::kCompute);
	for (uint32_t frameIt = 0; frameIt < SHADER_TYPES_FRAME_COUNT; frameIt++)
		pipeline.SetDescriptorData(
			"gTextures",
			ImageBinding{.sampler = {}, .imageView = myGraph.GetView(myColor), .layout = ImageLayout::kShaderReadOnly},
			DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES,
			SHADER_TYPES_RENDER_TARGET_TEXTURE_BASE + frameIt);
	pipeline.SetDescriptorData(
		"gTextures",
		ImageBinding{.sampler = {}, .imageView = myGraph.GetView(myTransmission), .layout = ImageLayout::kShaderReadOnly},
		DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES,
		SHADER_TYPES_TRANSMISSION_TEXTURE);
	for (auto [name, buffer] : std::array{
			 std::pair{"gOitHeads", myOitHeads}, std::pair{"gOitNodes", myOitNodes}, std::pair{"gOitCounter", myOitCounter}})
		pipeline.SetDescriptorData(
			name, BufferBinding{.buffer = myGraph.GetBuffer(buffer), .offset = 0}, DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);
}

void Renderer::Prepare(CommandBufferHandle cmd)
{
	// the transmission texture is only written in frames with transmissive materials, but bound in every one
	myGraph.GetImage(myTransmission).Transition(cmd, ImageLayout::kShaderReadOnly, ImageAspect::kColor);
}

void Renderer::Record(CommandBufferHandle cmd, Pipeline& pipeline, const FrameInputs& inputs)
{
	ZoneScopedN("Renderer::Record");

	ENSURE(inputs.swapchain && inputs.swapchainFrame && inputs.exposureHistogram && inputs.graphicsQueue);

	FrameInputs frame = inputs;
	frame.pushConstants.framebufferWidth = myGraph.GetExtent().width;
	frame.pushConstants.oitNodeCapacity = myOitNodeCapacity;
	myInputs = &frame;
	myPipeline = &pipeline;

	// the scene's draws: in two phases if it has transmissive materials
	myTwoPhases = false;
	if (frame.model != nullptr && frame.transmissive)
	{
		const auto& materials = frame.model->GetDesc().materials;
		for (size_t materialIt = 0; materialIt < materials.size(); materialIt++)
			myTwoPhases |= !materials[materialIt].blend && frame.transmissive(materialIt);
	}
	for (auto phase : {MainPassPhase::kOpaque, MainPassPhase::kTransmissive})
		myDrawLists[std::to_underlying(phase)] = frame.model != nullptr
			? BuildDrawList(*frame.model, phase, myTwoPhases, frame.transmissive ? frame.transmissive : [](size_t) { return false; })
			: DrawList{};
	myGraph.SetEnabled(myTransmissionPass, myTwoPhases);
	myGraph.SetEnabled(myTransmissivePhase, myTwoPhases);

	myGraph.Bind(mySwapchain, *frame.swapchain, 0);
	myGraph.Bind(myExposureHistogram, *frame.exposureHistogram);
	myGraph.Execute(cmd);

	myInputs = nullptr;
	myPipeline = nullptr;
}

void Renderer::InternalDrawMainPass(FrameGraph::PassContext& context, Pipeline& pipeline, const FrameInputs& inputs, MainPassPhase phase)
{
	auto& graphicsQueue = *inputs.graphicsQueue;
	auto& device = pipeline.GetDevice();

	pipeline.SetRenderTarget(*context.target);
	pipeline.BindLayoutAuto(device.GetPipelineLayoutHandle("Main"), PipelineBindPoint::kGraphics);

	const auto& list = myDrawLists[std::to_underlying(phase)];
	if (inputs.model == nullptr)
		return;

	ZoneScopedN("Renderer::drawViews");

	const auto grid = inputs.grid;
	uint32_t drawCount = grid.x * grid.y;
	uint32_t drawThreadCount = std::min<uint32_t>(drawCount, graphicsQueue.GetPool().GetDesc().levelCount);
	std::atomic_uint32_t drawAtomic = 0;
	const auto extent = context.target->GetExtent();
	uint32_t deltaX = extent.width / grid.x;
	uint32_t deltaY = extent.height / grid.y;
	ASSERT(deltaX > 0);
	ASSERT(deltaY > 0);

	// the views, in secondary command buffers
	for (uint32_t threadIt = 0; threadIt < drawThreadCount; threadIt++)
	{
		auto drawIt = drawAtomic++;
		if (drawIt >= drawCount)
			break;

		auto cmd = graphicsQueue.GetPool().SecondaryCommands(threadIt + 1, *context.beginInfo);
		CommandEncoder encoder(cmd);

		// vertices are pulled from gVertexBuffer by SV_VertexID, so there is no vertex input state to bind
		encoder.BindIndexBuffer(*list.indexBuffer, 0, IndexType::kUint32);
		pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);
		pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_GLOBAL_SAMPLERS);
		pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES);
		pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_VIEW);
		pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_MATERIAL);
		pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES);
		pipeline.BindPipelineAuto(cmd, {.blend = kOpaqueBlend});

		for (; drawIt < drawCount; drawIt = drawAtomic++)
		{
			ZoneScopedN("Renderer::drawView");

			// the view's viewport: its grid cell, letterboxed to its camera's aspect ratio
			auto viewIt = static_cast<uint16_t>(drawIt);
			auto posX = static_cast<int32_t>((viewIt % grid.x) * deltaX);
			auto posY = static_cast<int32_t>((viewIt / grid.x) * deltaY);
			auto width = deltaX;
			auto height = deltaY;
			if (viewIt < inputs.viewports.size() && inputs.viewports[viewIt].width > 0 && inputs.viewports[viewIt].height > 0)
			{
				posX = inputs.viewports[viewIt].x;
				posY = inputs.viewports[viewIt].y;
				width = inputs.viewports[viewIt].width;
				height = inputs.viewports[viewIt].height;
			}
			encoder.SetViewport(Viewport{
				.x = static_cast<float>(posX),
				.y = static_cast<float>(posY),
				.width = static_cast<float>(width),
				.height = static_cast<float>(height),
				.minDepth = 0.0F,
				.maxDepth = 1.0F});
			encoder.SetScissor(rhi::Rect{.x = posX, .y = posY, .width = width, .height = height});

			RecordDrawList(cmd, pipeline, list, inputs.pushConstants, viewIt);
		}

		cmd.End();
	}

	for (uint32_t threadIt = 1; threadIt <= drawThreadCount; threadIt++)
		graphicsQueue.Execute(threadIt, inputs.graphicsTimeline);
}

} // namespace gfx
