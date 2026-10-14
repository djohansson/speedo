#include "renderer.h"

#include <gfx/model.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <numeric>
#include <optional>
#include <print>
#include <string_view>

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

Renderer::Renderer(Device& device, RenderOutput& output)
	: myRenderOutput(output)
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
	// the light scattering materials diffuse into their volume, and the material (+ 1, 0 is none) in alpha, which the
	// main pass blurs over their surfaces (SHADER_TYPES_SCATTER_TEXTURE), and their depth (SHADER_TYPES_SCATTER_DEPTH_TEXTURE)
	myScatter = myGraph.CreateImage({.name = "Scatter", .format = Format::kR16G16B16A16Sfloat});
	myScatterDepth = myGraph.CreateImage({.name = "Scatter Depth", .format = Format::kD32Sfloat, .aspect = ImageAspect::kDepth});
	// and their multiscatter color, which the blur's profile is scaled by (SHADER_TYPES_SCATTER_ALBEDO_TEXTURE): srgb, for
	// the dark ones' precision
	myScatterAlbedo = myGraph.CreateImage({.name = "Scatter Albedo", .format = Format::kR8G8B8A8Srgb});
	// and the depth of their nearest back faces, which their thickness is measured by (SHADER_TYPES_SCATTER_BACK_DEPTH_TEXTURE)
	myScatterBackDepth = myGraph.CreateImage({.name = "Scatter Back Depth", .format = Format::kD32Sfloat, .aspect = ImageAspect::kDepth});
	// the scattering light blurred, with noise (SHADER_TYPES_SCATTER_NOISY_TEXTURE), and denoised
	// (SHADER_TYPES_SCATTER_LIGHT_TEXTURE), which the main pass adds
	myScatterNoisy = myGraph.CreateImage({.name = "Scatter Noisy", .format = Format::kR16G16B16A16Sfloat});
	myScatterLight = myGraph.CreateImage({.name = "Scatter Light", .format = Format::kR16G16B16A16Sfloat});
	// the ui's frosted panels' backdrop, before and after its blur along x (SHADER_TYPES_UI_BLUR_SOURCE_TEXTURE): the
	// frame's size (only the panels and the blur's reach around them are used), placed over the transients the frame is
	// done with by then
	myUiBlurSource = myGraph.CreateImage({.name = "UI Blur Source", .format = Format::kR16G16B16A16Sfloat});
	myUiBlurRows = myGraph.CreateImage({.name = "UI Blur Rows", .format = Format::kR16G16B16A16Sfloat});
	// the order independent transparency's per pixel lists (see OitNode): their heads (a node index per pixel of the
	// render target), nodes and counter
	myOitHeads = myGraph.CreateBuffer(
		{.name = "OIT Heads", .sizeOf = [](Extent2d extent) { return PixelCount(extent) * sizeof(uint32_t); }});
	myOitNodes = myGraph.CreateBuffer(
		{.name = "OIT Nodes", .sizeOf = [](Extent2d extent) { return std::max<uint64_t>(OitNodeCapacity(extent), 1) * sizeof(OitNode); }});
	myOitCounter = myGraph.CreateBuffer({.name = "OIT Counter", .size = sizeof(uint32_t)});
	myOutput = myGraph.ImportImage("Output");
	myShadowAtlas = myGraph.CreateImage(
		{.name = "Shadow Atlas",
		 .format = Format::kD32Sfloat,
		 .aspect = ImageAspect::kDepth,
		 .extent = {.width = shadows::kAtlasSize, .height = shadows::kAtlasSize}});
	for (uint32_t frameIt = 0; frameIt < SHADER_TYPES_FRAME_COUNT; frameIt++)
	{
		myShadowViewBuffers.emplace_back(BufferCreateDesc{
			device.CreateDeviceObjectCreateDesc(std::format("Shadow Views {}", frameIt)),
			SHADER_TYPES_SHADOW_VIEW_COUNT * sizeof(ShadowData),
			BufferUsage::kStorage,
			MemoryProperty::kHostVisible});
		myLightShadowBuffers.emplace_back(BufferCreateDesc{
			device.CreateDeviceObjectCreateDesc(std::format("Light Shadows {}", frameIt)),
			SHADER_TYPES_LIGHT_COUNT * sizeof(uint32_t),
			BufferUsage::kStorage,
			MemoryProperty::kHostVisible});
		myClusterViewBuffers.emplace_back(BufferCreateDesc{
			device.CreateDeviceObjectCreateDesc(std::format("Cluster Views {}", frameIt)),
			SHADER_TYPES_VIEW_COUNT * sizeof(ClusterView),
			BufferUsage::kStorage,
			MemoryProperty::kHostVisible});
		myClusterBuffers.emplace_back(BufferCreateDesc{
			device.CreateDeviceObjectCreateDesc(std::format("Clusters {}", frameIt)),
			SHADER_TYPES_VIEW_COUNT * SHADER_TYPES_CLUSTER_COUNT * 2 * sizeof(uint32_t),
			BufferUsage::kStorage,
			MemoryProperty::kHostVisible});
		myClusterLightBuffers.emplace_back(BufferCreateDesc{
			device.CreateDeviceObjectCreateDesc(std::format("Cluster Lights {}", frameIt)),
			SHADER_TYPES_CLUSTER_LIGHT_INDEX_COUNT * sizeof(uint32_t),
			BufferUsage::kStorage,
			MemoryProperty::kHostVisible});
		myUiBlurRectBuffers.emplace_back(BufferCreateDesc{
			device.CreateDeviceObjectCreateDesc(std::format("UI Blur Rects {}", frameIt)),
			SHADER_TYPES_UI_BLUR_RECT_COUNT * sizeof(UiBlurRect),
			BufferUsage::kStorage,
			MemoryProperty::kHostVisible});
	}
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

	// the lights' shadows: each of their views in its tile of the atlas
	myShadowPass = myGraph.AddPass(
		"Shadows",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.DepthAttachment(myShadowAtlas, LoadOp::kClear);
		},
		[this](FrameGraph::PassContext& context)
		{
			const auto& inputs = *myFrame.inputs;
			auto& pipeline = *myFrame.pipeline;
			auto cmd = context.cmd;
			CommandEncoder encoder(cmd);

			pipeline.SetRenderTarget(*context.target);
			pipeline.BindLayoutAuto(pipeline.GetLayoutHandle("Main"), PipelineBindPoint::kGraphics);
			encoder.BindIndexBuffer(*myShadowDrawList.indexBuffer, 0, IndexType::kUint32);
			pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);
			pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_GLOBAL_SAMPLERS);
			pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES);
			pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_VIEW);
			pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_MATERIAL);
			pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES);
			pipeline.BindPipelineAuto(cmd, {.blend = kOpaqueBlend});

			auto pushConstants = inputs.pushConstants;
			for (uint32_t viewIt = 0; viewIt < myShadowPlan.views.size(); viewIt++)
			{
				const auto& tile = myShadowPlan.tiles[viewIt];
				encoder.SetViewport(Viewport{
					.x = static_cast<float>(tile.x),
					.y = static_cast<float>(tile.y),
					.width = static_cast<float>(tile.size),
					.height = static_cast<float>(tile.size),
					.minDepth = 0.0F,
					.maxDepth = 1.0F});
				encoder.SetScissor(rhi::Rect{
					.x = static_cast<int32_t>(tile.x), .y = static_cast<int32_t>(tile.y), .width = tile.size, .height = tile.size});
				pushConstants.shadowView = viewIt;
				auto frustum = Frustum::FromViewProjection(glm::make_mat4(&myShadowPlan.views[viewIt].viewProjection[0][0]));
				RecordDrawList(cmd, pipeline, myShadowDrawList, pushConstants, 0, inputs.culling ? &frustum : nullptr);
			}
		});

	// the scattering materials' back faces, depth only: how thick they are where the Scatter pass sees them
	myScatterBackPass = myGraph.AddPass(
		"Scatter Back",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.DepthAttachment(myScatterBackDepth, LoadOp::kClear);
			builder.Contents(SubpassContents::kSecondaryCommandBuffers);
		},
		[this](FrameGraph::PassContext& context) { InternalDrawViews(context, *myFrame.pipeline, *myFrame.inputs, myScatterBackDrawList); });

	// the scattering materials' surfaces, lit as the light entering them (FragmentScatter), which the main pass blurs
	// over them by their volume's radius (see SubsurfaceScattering in the shaders)
	myScatterPass = myGraph.AddPass(
		"Scatter",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.ColorAttachment(myScatter, LoadOp::kClear, StoreOp::kStore, ClearValue{.color = {0.0F, 0.0F, 0.0F, 0.0F}});
			builder.ColorAttachment(myScatterAlbedo, LoadOp::kClear, StoreOp::kStore, ClearValue{.color = {0.0F, 0.0F, 0.0F, 0.0F}});
			builder.DepthAttachment(myScatterDepth, LoadOp::kClear);
			builder.Use(myScatterBackDepth, ResourceUse::Sampled(PipelineStage::kFragmentShader));
			builder.Use(myShadowAtlas, ResourceUse::Sampled(PipelineStage::kFragmentShader));
			builder.Contents(SubpassContents::kSecondaryCommandBuffers);
		},
		[this](FrameGraph::PassContext& context) { InternalDrawViews(context, *myFrame.pipeline, *myFrame.inputs, myScatterDrawList); });

	// the scattering light blurred over the surfaces by their diffusion profile, once per pixel (ComputeScatter), then
	// denoised (ComputeScatterDenoise): Main layout compute kernels over the render target
	auto kernel = [this](CommandBufferHandle cmd, ComputePipelineVariant variant, uint32_t width, uint32_t height)
	{
		auto& pipeline = *myFrame.pipeline;
		pipeline.BindLayoutAuto(pipeline.GetLayoutHandle("Main"), PipelineBindPoint::kCompute);
		pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES);
		pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_GLOBAL_RW_TEXTURES);
		pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);
		pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_GLOBAL_SAMPLERS);
		pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_VIEW);
		pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_MATERIAL);
		pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES);
		pipeline.BindPipelineAuto(cmd, variant);
		auto pushConstants = myFrame.inputs->pushConstants;
		pipeline.PushConstants(cmd, std::as_bytes(std::span(&pushConstants, 1)));
		CommandEncoder(cmd).DispatchThreads(pipeline.GetComputeLaunchParameters(variant), width, height);
	};
	auto scatterKernel = [this, kernel](CommandBufferHandle cmd, ComputePipelineVariant variant)
	{
		auto extent = myGraph.GetExtent();
		kernel(cmd, variant, extent.width, extent.height);
	};
	myScatterBlurPass = myGraph.AddPass(
		"Scatter Blur",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myScatter, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myScatterDepth, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myScatterAlbedo, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myScatterNoisy, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderWrite));
		},
		[this, scatterKernel](FrameGraph::PassContext& context)
		{
			scatterKernel(context.cmd, ComputePipelineVariant{.entryPoint = 1});
		});
	myScatterDenoisePass = myGraph.AddPass(
		"Scatter Denoise",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myScatterNoisy, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myScatterDepth, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myScatterLight, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderWrite));
		},
		[this, scatterKernel](FrameGraph::PassContext& context)
		{
			scatterKernel(context.cmd, ComputePipelineVariant{.entryPoint = 2});
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
				builder.Use(myShadowAtlas, ResourceUse::Sampled(PipelineStage::kFragmentShader));
				if (phase == MainPassPhase::kTransmissive)
					builder.Use(myTransmission, ResourceUse::Sampled(PipelineStage::kFragmentShader));
				builder.Use(myScatterLight, ResourceUse::Sampled(PipelineStage::kFragmentShader));
				builder.Contents(SubpassContents::kSecondaryCommandBuffers);
			},
			[this, phase](FrameGraph::PassContext& context)
			{ InternalDrawViews(context, *myFrame.pipeline, *myFrame.inputs, myDrawLists[std::to_underlying(phase)]); });
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
				context.graph.GetBuffer(myExposureHistogram), myFrame.inputs->frameIndex * kHistogramSize, kHistogramSize, 0);
		});

	// the backdrop, the transparency's resolve, exposure and tonemapping, into the swapchain image
	myGraph.AddPass(
		"ComputeMain",
		[this, &useOitLists](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myColor, ResourceUse::Sampled(PipelineStage::kComputeShader));
			useOitLists(builder, PipelineStage::kComputeShader, Access::kShaderRead);
			builder.Use(myExposureHistogram, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite));
			builder.Use(myOutput, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderWrite));
			builder.Use(myUiBlurSource, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderWrite));
		},
		[this](FrameGraph::PassContext& context)
		{
			const auto& inputs = *myFrame.inputs;
			auto& pipeline = *myFrame.pipeline;
			auto cmd = context.cmd;

			pipeline.BindLayoutAuto(pipeline.GetLayoutHandle("Main"), PipelineBindPoint::kCompute);
			pipeline.SetDescriptorData(
				"gRWTextures",
				ImageBinding{
					.sampler = {},
					.imageView = myRenderOutput.GetTarget(inputs.frameIndex).GetAttachments()[0],
					.layout = myRenderOutput.GetTarget(inputs.frameIndex).GetLayout(0)},
				DESCRIPTOR_SET_CATEGORY_GLOBAL_RW_TEXTURES,
				inputs.frameIndex);

			pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES);
			pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_GLOBAL_RW_TEXTURES);
			// the backdrop's: the views, the environment and its sampler; and the transparency lists
			pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);
			pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_GLOBAL_SAMPLERS);
			pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_VIEW);
			pipeline.BindDescriptorSetAuto(cmd, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES);
			pipeline.BindPipelineAuto(cmd, ComputePipelineVariant{});

			auto pushConstants = inputs.pushConstants;
			pipeline.PushConstants(cmd, std::as_bytes(std::span(&pushConstants, 1)));

			// cover the whole swapchain image
			auto extent = myRenderOutput.GetTarget(inputs.frameIndex).GetExtent();
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

	// the ui's frosted panels (the menu bar, menus and popups, as macOS's): the output under them blurred, along x then y,
	// before the ui draws its translucent panels over it. a thread per pixel of each panel (see UiBlurRect), only while
	// there are any
	myUiBlurXPass = myGraph.AddPass(
		"UI Blur X",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myUiBlurSource, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myUiBlurRows, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderWrite));
		},
		[this, kernel](FrameGraph::PassContext& context)
		{
			kernel(context.cmd, ComputePipelineVariant{.entryPoint = 3}, myUiBlurThreadsX, 1);
		});
	myUiBlurYPass = myGraph.AddPass(
		"UI Blur Y",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myUiBlurSource, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myUiBlurRows, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myOutput, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderWrite));
		},
		[this, kernel](FrameGraph::PassContext& context)
		{
			kernel(context.cmd, ComputePipelineVariant{.entryPoint = 4}, myUiBlurThreadsY, 1);
		});

	// what ends the frame: the window's ui and present, or an image's hand-off to sampling
	output.AddFinalPasses(myGraph, myOutput, myFrame);

	// the passes' gpu timings (see GetPassTimings)
	const char* collect = std::getenv("SPEEDO_GPU_TIMINGS");
	myCollectTimings = collect != nullptr && std::string_view(collect) == "1";
}

Renderer::~Renderer()
{
	// the medians of the passes' timings, for experiments (SPEEDO_GPU_TIMINGS=1)
	if (!myCollectedTimings.empty())
	{
		std::println("gpu pass timings (median ms over the frames that ran them):");
		for (auto& [name, samples] : myCollectedTimings)
		{
			std::ranges::sort(samples);
			std::println("  {}: {:.3f} ({} frames)", name, samples[samples.size() / 2], samples.size());
		}
	}
}

LightCapture Renderer::GetLightCapture() const
{
	return myLightCapture.Read().Get();
}

std::vector<std::pair<std::string, double>> Renderer::GetPassTimings() const
{
	return myPassTimings.Read().Get();
}

void Renderer::Resize(Device& device, Pipeline& pipeline, Extent2d extent)
{
	ZoneScopedN("Renderer::Resize");

	if (!myGraph.Compile(device, extent))
		return;

	myOitNodeCapacity = renderer::OitNodeCapacity(extent);

	// a dirty set is updated as a whole, so every frame's slot is written up front, with the layouts the passes leave
	// them in (so that later writes of the same are skipped as unchanged)
	pipeline.BindLayoutAuto(pipeline.GetLayoutHandle("Main"), PipelineBindPoint::kCompute);
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
	pipeline.SetDescriptorData(
		"gTextures",
		ImageBinding{.sampler = {}, .imageView = myGraph.GetView(myShadowAtlas), .layout = ImageLayout::kShaderReadOnly},
		DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES,
		SHADER_TYPES_SHADOW_ATLAS_TEXTURE);
	for (auto [image, slot] : std::array{
			 std::pair{myScatter, SHADER_TYPES_SCATTER_TEXTURE},
			 std::pair{myScatterDepth, SHADER_TYPES_SCATTER_DEPTH_TEXTURE},
			 std::pair{myScatterBackDepth, SHADER_TYPES_SCATTER_BACK_DEPTH_TEXTURE},
			 std::pair{myScatterAlbedo, SHADER_TYPES_SCATTER_ALBEDO_TEXTURE},
			 std::pair{myScatterNoisy, SHADER_TYPES_SCATTER_NOISY_TEXTURE},
			 std::pair{myScatterLight, SHADER_TYPES_SCATTER_LIGHT_TEXTURE},
			 std::pair{myUiBlurSource, SHADER_TYPES_UI_BLUR_SOURCE_TEXTURE},
			 std::pair{myUiBlurRows, SHADER_TYPES_UI_BLUR_ROWS_TEXTURE}})
		pipeline.SetDescriptorData(
			"gTextures",
			ImageBinding{.sampler = {}, .imageView = myGraph.GetView(image), .layout = ImageLayout::kShaderReadOnly},
			DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES,
			slot);
	for (auto [image, slot] : std::array{
			 std::pair{myScatterNoisy, SHADER_TYPES_SCATTER_NOISY_RW_TEXTURE},
			 std::pair{myScatterLight, SHADER_TYPES_SCATTER_LIGHT_RW_TEXTURE},
			 std::pair{myUiBlurSource, SHADER_TYPES_UI_BLUR_SOURCE_RW_TEXTURE},
			 std::pair{myUiBlurRows, SHADER_TYPES_UI_BLUR_ROWS_RW_TEXTURE}})
		pipeline.SetDescriptorData(
			"gRWTextures",
			ImageBinding{.sampler = {}, .imageView = myGraph.GetView(image), .layout = ImageLayout::kGeneral},
			DESCRIPTOR_SET_CATEGORY_GLOBAL_RW_TEXTURES,
			slot);
	for (uint32_t frameIt = 0; frameIt < SHADER_TYPES_FRAME_COUNT; frameIt++)
	{
		pipeline.SetDescriptorData(
			"gShadowViews",
			BufferBinding{.buffer = myShadowViewBuffers[frameIt], .offset = 0},
			DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES,
			frameIt);
		pipeline.SetDescriptorData(
			"gLightShadows",
			BufferBinding{.buffer = myLightShadowBuffers[frameIt], .offset = 0},
			DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES,
			frameIt);
		for (auto [name, buffers] : std::array{
				 std::pair{"gClusterViews", &myClusterViewBuffers},
				 std::pair{"gClusters", &myClusterBuffers},
				 std::pair{"gClusterLights", &myClusterLightBuffers},
				 std::pair{"gUiBlurRects", &myUiBlurRectBuffers}})
			pipeline.SetDescriptorData(
				name, BufferBinding{.buffer = (*buffers)[frameIt], .offset = 0}, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES, frameIt);
	}
	for (auto [name, buffer] : std::array{
			 std::pair{"gOitHeads", myOitHeads}, std::pair{"gOitNodes", myOitNodes}, std::pair{"gOitCounter", myOitCounter}})
		pipeline.SetDescriptorData(
			name, BufferBinding{.buffer = myGraph.GetBuffer(buffer), .offset = 0}, DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);
}

void Renderer::Prepare(CommandBufferHandle cmd)
{
	// the transmission texture is only written in frames with transmissive materials, but bound in every one (as are the
	// shadow atlas, in frames with shadows, and the scatter targets, in frames with scattering materials)
	myGraph.GetImage(myTransmission).Transition(cmd, ImageLayout::kShaderReadOnly, ImageAspect::kColor);
	myGraph.GetImage(myShadowAtlas).Transition(cmd, ImageLayout::kShaderReadOnly, ImageAspect::kDepth);
	myGraph.GetImage(myScatter).Transition(cmd, ImageLayout::kShaderReadOnly, ImageAspect::kColor);
	myGraph.GetImage(myScatterDepth).Transition(cmd, ImageLayout::kShaderReadOnly, ImageAspect::kDepth);
	myGraph.GetImage(myScatterBackDepth).Transition(cmd, ImageLayout::kShaderReadOnly, ImageAspect::kDepth);
	myGraph.GetImage(myScatterAlbedo).Transition(cmd, ImageLayout::kShaderReadOnly, ImageAspect::kColor);
	myGraph.GetImage(myScatterNoisy).Transition(cmd, ImageLayout::kShaderReadOnly, ImageAspect::kColor);
	myGraph.GetImage(myScatterLight).Transition(cmd, ImageLayout::kShaderReadOnly, ImageAspect::kColor);
}

void Renderer::Record(CommandBufferHandle cmd, Pipeline& pipeline, const FrameInputs& inputs)
{
	ZoneScopedN("Renderer::Record");

	ENSURE(inputs.exposureHistogram && inputs.graphicsQueue);

	// the passes' timings collected since the last frame (see GpuProfiler::Collect, in the frame loop): the ui's smoothed
	// (an exponential moving average over the frames that timed a pass, which may be few: the backend may not write every
	// timestamp), and shown until a pass hasn't been timed for a while, so that the list doesn't flicker
	if (auto* profiler = inputs.graphicsQueue->GetProfiler())
	{
		constexpr double kSmoothing = 0.1;
		constexpr uint64_t kShownRecords = 240; // 2 seconds at 120 frames a second
		myRecordCount++;
		for (const auto& [name, milliseconds] : profiler->TakeTimings(myGraph.GetTimingGroup()))
		{
			auto it = std::ranges::find(mySmoothedTimings, name, [](const auto& timing) { return std::string_view(timing.name); });
			if (it == mySmoothedTimings.end())
				it = mySmoothedTimings.insert(mySmoothedTimings.end(), {.name = std::string(name), .milliseconds = milliseconds});
			else
				it->milliseconds += kSmoothing * (milliseconds - it->milliseconds);
			it->lastRecord = myRecordCount;
			if (myCollectTimings)
			{
				auto collected = std::ranges::find(myCollectedTimings, name, [](const auto& entry) { return std::string_view(entry.first); });
				if (collected == myCollectedTimings.end())
					collected = myCollectedTimings.insert(myCollectedTimings.end(), {std::string(name), {}});
				collected->second.push_back(milliseconds);
			}
		}
		std::vector<std::pair<std::string, double>> shown;
		for (const auto& timing : mySmoothedTimings)
			if (myRecordCount - timing.lastRecord <= kShownRecords)
				shown.emplace_back(timing.name, timing.milliseconds);
		myPassTimings.Write().Get() = std::move(shown);
	}

	FrameInputs frame = inputs;
	frame.pushConstants.framebufferWidth = myGraph.GetExtent().width;
	frame.pushConstants.oitNodeCapacity = myOitNodeCapacity;
	myFrame.inputs = &frame;
	myFrame.pipeline = &pipeline;

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
			? BuildDrawList(
				  *frame.model, phase, myTwoPhases, frame.transmissive ? frame.transmissive : [](size_t) { return false; }, frame.specialization)
			: DrawList{};
	// the shadows: planned for the frame, and written for the shader to read once the frame's previous use of its
	// buffers is done (the caller waited for its fence)
	myShadowPlan = frame.shadows && frame.model != nullptr
		? shadows::Plan(frame.lights.first(std::min<size_t>(frame.lights.size(), SHADER_TYPES_LIGHT_COUNT)), frame.camera, frame.sceneBounds)
		: shadows::ShadowPlan{};
	myShadowPlan.lightShadows.resize(SHADER_TYPES_LIGHT_COUNT, SHADER_TYPES_NO_SHADOW);
	ENSURE(myShadowPlan.views.size() <= SHADER_TYPES_SHADOW_VIEW_COUNT);
	{
		auto& views = myShadowViewBuffers[frame.frameIndex];
		auto memory = views.Map();
		std::ranges::copy(std::as_bytes(std::span(myShadowPlan.views)), memory.begin());
		if (!myShadowPlan.views.empty())
			views.Flush(0, myShadowPlan.views.size() * sizeof(ShadowData));
		views.Unmap();
		auto& lightShadows = myLightShadowBuffers[frame.frameIndex];
		memory = lightShadows.Map();
		std::ranges::copy(std::as_bytes(std::span(myShadowPlan.lightShadows)), memory.begin());
		lightShadows.Flush(0, myShadowPlan.lightShadows.size() * sizeof(uint32_t));
		lightShadows.Unmap();
	}
	// the lights' clusters per view, written as the shadows are
	{
		auto lights = frame.lights.first(std::min<size_t>(frame.lights.size(), SHADER_TYPES_LIGHT_COUNT));
		auto plan = frame.clusteredLights ? clusters::Plan(lights, frame.viewProjections, frame.sceneBounds)
										  : clusters::ClusterPlan{.views = std::vector<ClusterView>(SHADER_TYPES_VIEW_COUNT)};
		auto write = [](Buffer& buffer, std::span<const std::byte> bytes)
		{
			if (bytes.empty())
				return;
			auto memory = buffer.Map();
			std::ranges::copy(bytes, memory.begin());
			buffer.Flush(0, bytes.size());
			buffer.Unmap();
		};
		write(myClusterViewBuffers[frame.frameIndex], std::as_bytes(std::span(plan.views)));
		write(myClusterBuffers[frame.frameIndex], std::as_bytes(std::span(plan.ranges)));
		write(myClusterLightBuffers[frame.frameIndex], std::as_bytes(std::span(plan.lights)));
	}
	if (frame.captureLights)
	{
		auto capture = myLightCapture.Write();
		auto& lights = capture.Get();
		lights.lights.assign(frame.lights.begin(), frame.lights.end());
		lights.modelLightCount = frame.modelLightCount;
		lights.shadowViews = myShadowPlan.views;
		lights.lightShadows = myShadowPlan.lightShadows;
		lights.sceneBounds = frame.sceneBounds;
	}
	myShadowDrawList = !myShadowPlan.views.empty()
		? BuildShadowDrawList(*frame.model, frame.transmissive ? frame.transmissive : [](size_t) { return false; })
		: DrawList{};
	bool shadowsDrawn = !myShadowDrawList.Empty();
	frame.pushConstants.shadows = shadowsDrawn ? 1U : 0U;
	frame.pushConstants.shadowView = SHADER_TYPES_NO_SHADOW;
	myGraph.SetEnabled(myShadowPass, shadowsDrawn);

	// the scattering materials' light, if any are drawn
	myScatterDrawList = frame.model != nullptr && frame.scatters
		? BuildScatterDrawList(*frame.model, frame.scatters, frame.specialization)
		: DrawList{};
	myScatterBackDrawList = BuildScatterBackDrawList(myScatterDrawList);
	frame.pushConstants.scatter = myScatterDrawList.Empty() ? 0U : 1U;
	myGraph.SetEnabled(myScatterPass, !myScatterDrawList.Empty());
	myGraph.SetEnabled(myScatterBackPass, !myScatterDrawList.Empty());
	myGraph.SetEnabled(myScatterBlurPass, !myScatterDrawList.Empty());
	myGraph.SetEnabled(myScatterDenoisePass, !myScatterDrawList.Empty());
	// the ui's frosted panels: clipped to the frame, each a run of the UI Blur passes' threads, a pixel each (X's over
	// the blur's reach above and below too)
	{
		auto extent = myGraph.GetExtent();
		std::vector<UiBlurRect> rects;
		myUiBlurThreadsX = 0;
		myUiBlurThreadsY = 0;
		for (auto rect : frame.uiBlurRects)
		{
			if (rects.size() == SHADER_TYPES_UI_BLUR_RECT_COUNT)
				break;
			rect.rect[0] = std::clamp(std::floor(rect.rect[0]), 0.0F, static_cast<float>(extent.width));
			rect.rect[1] = std::clamp(std::floor(rect.rect[1]), 0.0F, static_cast<float>(extent.height));
			rect.rect[2] = std::clamp(std::ceil(rect.rect[2]), 0.0F, static_cast<float>(extent.width));
			rect.rect[3] = std::clamp(std::ceil(rect.rect[3]), 0.0F, static_cast<float>(extent.height));
			auto width = static_cast<uint32_t>(rect.rect[2] - rect.rect[0]);
			auto height = static_cast<uint32_t>(rect.rect[3] - rect.rect[1]);
			if (width == 0 || height == 0)
				continue;
			rect.firstThreadX = myUiBlurThreadsX;
			rect.firstThreadY = myUiBlurThreadsY;
			myUiBlurThreadsX += width * (height + (2 * SHADER_TYPES_UI_BLUR_RADIUS));
			myUiBlurThreadsY += width * height;
			rects.push_back(rect);
		}
		if (!rects.empty())
		{
			auto& buffer = myUiBlurRectBuffers[frame.frameIndex];
			auto memory = buffer.Map();
			std::ranges::copy(std::as_bytes(std::span(rects)), memory.begin());
			buffer.Flush(0, rects.size() * sizeof(UiBlurRect));
			buffer.Unmap();
		}
		frame.pushConstants.uiBlurRectCount = static_cast<uint32_t>(rects.size());
		myGraph.SetEnabled(myUiBlurXPass, !rects.empty());
		myGraph.SetEnabled(myUiBlurYPass, !rects.empty());
	}

	myGraph.SetEnabled(myTransmissionPass, myTwoPhases);
	myGraph.SetEnabled(myTransmissivePhase, myTwoPhases);

	myGraph.Bind(myOutput, myRenderOutput.GetTarget(frame.frameIndex), 0);
	myGraph.Bind(myExposureHistogram, *frame.exposureHistogram);
	myGraph.Execute(cmd, inputs.graphicsQueue->GetProfiler());

	myFrame.inputs = nullptr;
	myFrame.pipeline = nullptr;
}

void Renderer::InternalDrawViews(FrameGraph::PassContext& context, Pipeline& pipeline, const FrameInputs& inputs, const DrawList& list)
{
	auto& graphicsQueue = *inputs.graphicsQueue;
	auto& device = pipeline.GetDevice();

	pipeline.SetRenderTarget(*context.target);
	pipeline.BindLayoutAuto(pipeline.GetLayoutHandle("Main"), PipelineBindPoint::kGraphics);

	if (inputs.model == nullptr || list.indexBuffer == nullptr)
		return;

	ZoneScopedN("Renderer::drawViews");

	const auto grid = inputs.grid;
	uint32_t viewCount = grid.x * grid.y;
	const auto extent = context.target->GetExtent();
	uint32_t deltaX = extent.width / grid.x;
	uint32_t deltaY = extent.height / grid.y;
	ASSERT(deltaX > 0);
	ASSERT(deltaY > 0);

	// the views, in a secondary command buffer run inside the pass's render target
	auto cmd = graphicsQueue.GetPool().BeginSecondary(*context.beginInfo);
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

	for (uint32_t drawIt = 0; drawIt < viewCount; drawIt++)
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

		std::optional<Frustum> frustum;
		if (inputs.culling && viewIt < inputs.viewProjections.size())
			frustum = Frustum::FromViewProjection(inputs.viewProjections[viewIt]);
		RecordDrawList(cmd, pipeline, list, inputs.pushConstants, viewIt, frustum ? &*frustum : nullptr, inputs.runInBackground);
	}

	std::vector<CommandBuffer> secondaries;
	secondaries.push_back(std::move(cmd));
	CommandEncoder(context.cmd).ExecuteCommands(std::move(secondaries));
}

} // namespace gfx
