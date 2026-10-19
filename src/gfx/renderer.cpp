#include "renderer.h"

#include <gfx/model.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <bit>
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

// the gpu's culling: ComputeCull, the Main layout's sixth compute entry point. a job per draw list and view drawing it
// (the main pass's phases and the scatter passes' lists by the main views, the shadows' by the shadow views), and the
// views, main then shadow
constexpr uint8_t kCullEntryPoint = 5;
constexpr uint8_t kDepthPyramidEntryPoint = 6;
constexpr uint8_t kDepthSaveEntryPoint = 7;
constexpr uint8_t kDepthReprojectEntryPoint = 8;
constexpr uint32_t kCullJobCount = (4 * SHADER_TYPES_VIEW_COUNT) + SHADER_TYPES_SHADOW_VIEW_COUNT;
constexpr uint32_t kCullViewCount = SHADER_TYPES_VIEW_COUNT + SHADER_TYPES_SHADOW_VIEW_COUNT;

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
		myCullJobBuffers.emplace_back(BufferCreateDesc{
			device.CreateDeviceObjectCreateDesc(std::format("Cull Jobs {}", frameIt)),
			kCullJobCount * sizeof(CullJob),
			BufferUsage::kStorage,
			MemoryProperty::kHostVisible});
		myCullViewBuffers.emplace_back(BufferCreateDesc{
			device.CreateDeviceObjectCreateDesc(std::format("Cull Views {}", frameIt)),
			kCullViewCount * sizeof(CullView),
			BufferUsage::kStorage,
			MemoryProperty::kHostVisible});
		myCullStatisticsBuffers.emplace_back(BufferCreateDesc{
			device.CreateDeviceObjectCreateDesc(std::format("Cull Statistics {}", frameIt)),
			CULL_STATISTIC_COUNT * sizeof(uint32_t),
			BufferUsage::kStorage,
			MemoryProperty::kHostVisible});
		myDrawStreams.emplace_back(device);
	}
	myDrawRecords = {.name = "Draw Records", .binding = "gDrawRecords", .elementSize = sizeof(DrawRecord)};
	InternalCreate(myDrawRecords, device, SHADER_TYPES_DRAW_RECORD_COUNT);
	myCullCandidates = {.name = "Cull Candidates", .binding = "gCullCandidates", .elementSize = sizeof(CullCandidate)};
	InternalCreate(myCullCandidates, device, SHADER_TYPES_DRAW_RECORD_COUNT);
	myCullCommandBuffers = {
		.name = "Cull Commands",
		.binding = "gCullCommands",
		.elementSize = sizeof(DrawIndexedIndirectCommand),
		.usage = BufferUsage::kStorage | BufferUsage::kIndirect,
		.memory = MemoryProperty::kDeviceLocal};
	InternalCreate(myCullCommandBuffers, device, SHADER_TYPES_DRAW_RECORD_COUNT);
	myCullCountBuffers = {
		.name = "Cull Counts",
		.binding = "gCullCounts",
		.elementSize = sizeof(uint32_t),
		.usage = BufferUsage::kStorage | BufferUsage::kIndirect | BufferUsage::kTransferDestination,
		.memory = MemoryProperty::kDeviceLocal};
	InternalCreate(myCullCountBuffers, device, SHADER_TYPES_DRAW_RECORD_COUNT);
	myExposureHistogram = myGraph.ImportBuffer("Exposure Histogram");
	myCullCommands = myGraph.ImportBuffer("Cull Commands");
	myCullCounts = myGraph.ImportBuffer("Cull Counts");
	myCullVisibility = myGraph.ImportBuffer("Cull Visibility");
	myCullStatisticsId = myGraph.ImportBuffer("Cull Statistics");
	myCullVisibilityCapacity = SHADER_TYPES_DRAW_RECORD_COUNT;
	myCullVisibilityBuffer = Buffer(BufferCreateDesc{
		device.CreateDeviceObjectCreateDesc("Cull Visibility"),
		myCullVisibilityCapacity * sizeof(uint32_t),
		BufferUsage::kStorage,
		MemoryProperty::kDeviceLocal});
	// the main depth's farthest per texel, by level (see ComputeDepthPyramid), which occlusion culling tests against
	myDepthPyramid = myGraph.CreateImage({.name = "Depth Pyramid", .format = Format::kR32Sfloat, .mipChain = true, .downscale = 2});
	// the depth reprojection's (see ComputeDepthReproject): the last frame's depth, kept (see Resize), and the frame's
	// reprojected
	myPreviousDepth = myGraph.ImportBuffer("Previous Depth");
	myReprojectedDepth = myGraph.CreateBuffer(
		{.name = "Reprojected Depth", .sizeOf = [](Extent2d extent) { return PixelCount(extent) * sizeof(uint32_t); }});

	constexpr auto kOitAccess = Access::kShaderRead | Access::kShaderWrite;
	auto useOitLists = [this](FrameGraph::PassBuilder& builder, PipelineStage stages, Access access)
	{
		builder.Use(myOitHeads, ResourceUse::Storage(stages, access));
		builder.Use(myOitNodes, ResourceUse::Storage(stages, stages == PipelineStage::kFragmentShader ? Access::kShaderWrite : access));
		builder.Use(myOitCounter, ResourceUse::Storage(stages, access));
	};

	// Main layout compute kernels, of width by height threads
	auto kernel = [this](
					  CommandBufferHandle cmd,
					  ComputePipelineVariant variant,
					  uint32_t width,
					  uint32_t height,
					  uint32_t cullJobBase = 0,
					  uint32_t depthPyramidLevel = 0,
					  uint32_t depthPyramidSource = 0)
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
		pushConstants.cullJobBase = cullJobBase;
		pushConstants.depthPyramidLevel = depthPyramidLevel;
		pushConstants.depthPyramidSource = depthPyramidSource;
		pipeline.PushConstants(cmd, std::as_bytes(std::span(&pushConstants, 1)));
		CommandEncoder(cmd).DispatchThreads(pipeline.GetComputeLaunchParameters(variant), width, height);
	};

	// the gpu's culling: each draw list's draws tested against each view drawing it, those in view compacted into their
	// run's commands and counted, for the passes after to draw (see CullCandidate, RecordCulledDrawList). the counts
	// emptied first, once the previous frame's draws have read them
	myCullClearPass = myGraph.AddPass(
		"Cull Clear",
		[this](FrameGraph::PassBuilder& builder) { builder.Use(myCullCounts, ResourceUse::TransferDestination()); },
		[this](FrameGraph::PassContext& context)
		{
			CommandEncoder(context.cmd).FillBuffer(context.graph.GetBuffer(myCullCounts), 0, myCullCountCount * sizeof(uint32_t), 0);
		});
	// the depth pyramid's levels, from the main depth or the depth reprojected (each dispatch reads the one before:
	// barriers between them, within the pass)
	auto pyramid = [this, kernel](FrameGraph::PassContext& context, uint32_t source)
	{
		const auto& levels = context.graph.GetImage(myDepthPyramid).GetDesc().mipLevels;
		for (uint32_t levelIt = 0; levelIt < myDepthPyramidViews.size(); levelIt++)
		{
			if (levelIt > 0)
				CommandEncoder(context.cmd).Barrier(
					PipelineStage::kComputeShader, Access::kShaderWrite, PipelineStage::kComputeShader, Access::kShaderRead);
			kernel(
				context.cmd,
				ComputePipelineVariant{.entryPoint = kDepthPyramidEntryPoint},
				levels[levelIt].extent.width,
				levels[levelIt].extent.height,
				0,
				levelIt,
				source);
		}
	};
	// occlusion by the last frame's depth (see ComputeDepthReproject): the depth reprojected, from the far plane, and its
	// pyramid, which the Cull Reprojected pass culls by before the main pass draws
	myDepthReprojectClearPass = myGraph.AddPass(
		"Depth Reproject Clear",
		[this](FrameGraph::PassBuilder& builder) { builder.Use(myReprojectedDepth, ResourceUse::TransferDestination()); },
		[this](FrameGraph::PassContext& context)
		{
			constexpr uint32_t kFarPlane = 0x3F800000U; // 1.0F
			CommandEncoder(context.cmd).FillBuffer(context.graph.GetBuffer(myReprojectedDepth), 0, PixelCount(myGraph.GetExtent()) * sizeof(uint32_t), kFarPlane);
		});
	myDepthReprojectPass = myGraph.AddPass(
		"Depth Reproject",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myPreviousDepth, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead));
			builder.Use(myReprojectedDepth, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite));
		},
		[this, kernel](FrameGraph::PassContext& context)
		{
			kernel(context.cmd, ComputePipelineVariant{.entryPoint = kDepthReprojectEntryPoint}, myPreviousExtent.width, myPreviousExtent.height);
		});
	myDepthPyramidReprojectedPass = myGraph.AddPass(
		"Depth Pyramid Reprojected",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myReprojectedDepth, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead));
			builder.Use(myDepthPyramid, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderWrite));
		},
		[pyramid](FrameGraph::PassContext& context) { pyramid(context, 1); });
	myCullReprojectedPass = myGraph.AddPass(
		"Cull Reprojected",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myCullCounts, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite));
			builder.Use(myCullCommands, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderWrite));
			builder.Use(myCullStatisticsId, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite));
			builder.Use(myCullVisibility, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite));
			builder.Use(myDepthPyramid, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead));
		},
		[this, kernel](FrameGraph::PassContext& context)
		{
			kernel(context.cmd, ComputePipelineVariant{.entryPoint = kCullEntryPoint}, myCullMaxCandidateCount, myCullEarlyJobCount);
		});
	myCullPass = myGraph.AddPass(
		"Cull",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myCullCounts, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite));
			builder.Use(myCullCommands, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderWrite));
			builder.Use(myCullStatisticsId, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite));
			builder.Use(myCullVisibility, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead));
		},
		[this, kernel](FrameGraph::PassContext& context)
		{
			kernel(context.cmd, ComputePipelineVariant{.entryPoint = kCullEntryPoint}, myCullMaxCandidateCount, myCullEarlyJobCount);
		});
	auto useCulled = [this](FrameGraph::PassBuilder& builder)
	{
		builder.Use(myCullCommands, ResourceUse::Indirect());
		builder.Use(myCullCounts, ResourceUse::Indirect());
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
		[this, useCulled](FrameGraph::PassBuilder& builder)
		{
			builder.DepthAttachment(myShadowAtlas, LoadOp::kClear);
			useCulled(builder);
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
				if (auto culled = InternalCulledDraws(ListId::kShadow, viewIt))
				{
					RecordCulledDrawList(cmd, pipeline, myShadowDrawList, pushConstants, 0, *culled);
					continue;
				}
				auto frustum = Frustum::FromViewProjection(glm::make_mat4(&myShadowPlan.views[viewIt].viewProjection[0][0]));
				RecordDrawList(cmd, pipeline, myShadowDrawList, pushConstants, 0, *myDrawStream, inputs.culling ? &frustum : nullptr);
			}
		});

	// the scattering materials' back faces, depth only: how thick they are where the Scatter pass sees them
	myScatterBackPass = myGraph.AddPass(
		"Scatter Back",
		[this, useCulled](FrameGraph::PassBuilder& builder)
		{
			builder.DepthAttachment(myScatterBackDepth, LoadOp::kClear);
			useCulled(builder);
			builder.Contents(SubpassContents::kSecondaryCommandBuffers);
		},
		[this](FrameGraph::PassContext& context) { InternalDrawViews(context, *myFrame.pipeline, *myFrame.inputs, ListId::kScatterBack); });

	// the scattering materials' surfaces, lit as the light entering them (FragmentScatter), which the main pass blurs
	// over them by their volume's radius (see SubsurfaceScattering in the shaders)
	myScatterPass = myGraph.AddPass(
		"Scatter",
		[this, useCulled](FrameGraph::PassBuilder& builder)
		{
			useCulled(builder);
			builder.ColorAttachment(myScatter, LoadOp::kClear, StoreOp::kStore, ClearValue{.color = {0.0F, 0.0F, 0.0F, 0.0F}});
			builder.ColorAttachment(myScatterAlbedo, LoadOp::kClear, StoreOp::kStore, ClearValue{.color = {0.0F, 0.0F, 0.0F, 0.0F}});
			builder.DepthAttachment(myScatterDepth, LoadOp::kClear);
			builder.Use(myScatterBackDepth, ResourceUse::Sampled(PipelineStage::kFragmentShader));
			builder.Use(myShadowAtlas, ResourceUse::Sampled(PipelineStage::kFragmentShader));
			builder.Contents(SubpassContents::kSecondaryCommandBuffers);
		},
		[this](FrameGraph::PassContext& context) { InternalDrawViews(context, *myFrame.pipeline, *myFrame.inputs, ListId::kScatter); });

	// the scattering light blurred over the surfaces by their diffusion profile, once per pixel (ComputeScatter), then
	// denoised (ComputeScatterDenoise)
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
	auto mainPass = [&](MainPassPhase phase, bool late = false)
	{
		auto load = phase == MainPassPhase::kOpaque && !late ? LoadOp::kClear : LoadOp::kLoad;
		return myGraph.AddPass(
			late ? "Main Late" : (phase == MainPassPhase::kOpaque ? "Main" : "Main Transmissive"),
			[this, load, phase, &useOitLists, useCulled](FrameGraph::PassBuilder& builder)
			{
				useCulled(builder);
				builder.ColorAttachment(myColor, load, StoreOp::kStore, ClearValue{.color = {0.0F, 0.0F, 0.0F, 0.0F}});
				builder.DepthAttachment(myDepth, load);
				useOitLists(builder, PipelineStage::kFragmentShader, kOitAccess);
				builder.Use(myShadowAtlas, ResourceUse::Sampled(PipelineStage::kFragmentShader));
				if (phase == MainPassPhase::kTransmissive)
					builder.Use(myTransmission, ResourceUse::Sampled(PipelineStage::kFragmentShader));
				builder.Use(myScatterLight, ResourceUse::Sampled(PipelineStage::kFragmentShader));
				builder.Contents(SubpassContents::kSecondaryCommandBuffers);
			},
			[this, phase, late](FrameGraph::PassContext& context)
			{
				InternalDrawViews(
					context,
					*myFrame.pipeline,
					*myFrame.inputs,
					phase == MainPassPhase::kOpaque ? ListId::kMain : ListId::kMainTransmissive,
					late);
			});
	};
	mainPass(MainPassPhase::kOpaque);

	// occlusion culling: the depth pyramid of what the main pass drew first (the draws seen last frame), level by level
	// (each dispatch reads the one before: barriers between them, within the pass)
	myDepthPyramidPass = myGraph.AddPass(
		"Depth Pyramid",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myDepth, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myDepthPyramid, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderWrite));
		},
		[pyramid](FrameGraph::PassContext& context) { pyramid(context, 0); });
	// then the rest of the main pass's draws, those it doesn't hide (and the transmissive phase's)
	myCullLatePass = myGraph.AddPass(
		"Cull Late",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myCullCounts, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite));
			builder.Use(myCullCommands, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderWrite));
			builder.Use(myCullStatisticsId, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite));
			builder.Use(myCullVisibility, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite));
			builder.Use(myDepthPyramid, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead));
		},
		[this, kernel](FrameGraph::PassContext& context)
		{
			kernel(
				context.cmd,
				ComputePipelineVariant{.entryPoint = kCullEntryPoint},
				myCullLateMaxCandidateCount,
				static_cast<uint32_t>(myCullJobs.size()) - myCullEarlyJobCount,
				myCullEarlyJobCount);
		});
	myMainLatePass = mainPass(MainPassPhase::kOpaque, true);

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

	// the frame's depth of what doesn't move, for the next to reproject (see ComputeDepthSave)
	myDepthSavePass = myGraph.AddPass(
		"Depth Save",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myDepth, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myPreviousDepth, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderWrite));
		},
		[this, kernel](FrameGraph::PassContext& context)
		{
			auto extent = myGraph.GetExtent();
			kernel(context.cmd, ComputePipelineVariant{.entryPoint = kDepthSaveEntryPoint}, extent.width, extent.height);
		});

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
			builder.Use(myCullStatisticsId, ResourceUse::HostRead());
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
	// each run of draws with the same pipeline and state as one indirect draw (see RecordDrawList), unless
	// SPEEDO_INDIRECT_DRAWS=0: direct draws
	const char* indirect = std::getenv("SPEEDO_INDIRECT_DRAWS");
	for (auto& stream : myDrawStreams)
		stream.indirect = indirect == nullptr || std::string_view(indirect) != "0";
	// the gpu culls the draws (the Cull pass) where it counts them natively (DrawIndexedIndirectCount) and the draws are
	// indirect, else the cpu does: where the count is emulated (KosmicKrisp) it costs more than it saves (see CLAUDE.md).
	// SPEEDO_GPU_CULLING=0 or 1 overrides that, where the count exists at all
	auto limits = device.GetLimits();
	const char* gpuCulling = std::getenv("SPEEDO_GPU_CULLING");
	bool gpuCullingWanted = gpuCulling != nullptr ? std::string_view(gpuCulling) == "1" : !limits.drawIndirectCountEmulated;
	myGpuCulling = limits.drawIndirectCount && myDrawStreams.front().indirect && gpuCullingWanted;
	// occlusion culling, where the count exists (SPEEDO_OCCLUSION_CULLING=0 turns it off): it culls the main pass on the
	// gpu even where the count is emulated, since the main pass makes few counts (a run per view), and what it hides is
	// what the cpu can't
	const char* occlusion = std::getenv("SPEEDO_OCCLUSION_CULLING");
	myOcclusionCulling = limits.drawIndirectCount && myDrawStreams.front().indirect &&
						 (occlusion == nullptr || std::string_view(occlusion) != "0");
	myCountEmulated = limits.drawIndirectCountEmulated;
	if (const char* mode = std::getenv("SPEEDO_OCCLUSION_MODE"); mode != nullptr)
	{
		std::string_view name(mode);
		myOcclusionMode = name == "reprojected" ? OcclusionMode::kReprojected
						  : name == "hybrid"	? OcclusionMode::kHybrid
												: OcclusionMode::kTwoPhase;
	}
	const char* collect = std::getenv("SPEEDO_GPU_TIMINGS");
	myCollectTimings = collect != nullptr && std::string_view(collect) == "1";
}

Renderer::~Renderer()
{
	// what the occlusion culling decided, on average per frame (SPEEDO_GPU_TIMINGS=1)
	if (myCollectTimings && myCullStatisticFrameCount > 0)
	{
		auto mean = [this](uint32_t statistic)
		{ return static_cast<double>(myCullStatisticSums[statistic]) / static_cast<double>(myCullStatisticFrameCount); };
		std::println(
			"occlusion culling ({}), draws per frame: {:.1f} early, {:.1f} late ({:.1f} blended), {:.1f} hidden ({} frames)",
			GetOcclusionModeName(),
			mean(CULL_STATISTIC_EARLY),
			mean(CULL_STATISTIC_LATE) + mean(CULL_STATISTIC_LATE_BLENDED),
			mean(CULL_STATISTIC_LATE_BLENDED),
			mean(CULL_STATISTIC_HIDDEN),
			myCullStatisticFrameCount);
	}
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

std::array<uint32_t, CULL_STATISTIC_COUNT> Renderer::GetCullStatistics() const
{
	return myCullStatistics.Read().Get();
}

std::string_view Renderer::GetOcclusionModeName() const noexcept
{
	if (!myOcclusionCulling)
		return "off";
	switch (myOcclusionMode)
	{
	case OcclusionMode::kTwoPhase: return "two-phase";
	case OcclusionMode::kReprojected: return "reprojected";
	case OcclusionMode::kHybrid: return "hybrid";
	}
	return "";
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
				 std::pair{"gUiBlurRects", &myUiBlurRectBuffers},
				 std::pair{"gCullJobs", &myCullJobBuffers},
				 std::pair{"gCullStatistics", &myCullStatisticsBuffers},
				 std::pair{"gCullViews", &myCullViewBuffers},
				 std::pair{"gDrawRecords", &myDrawRecords.buffers},
				 std::pair{"gCullCandidates", &myCullCandidates.buffers},
				 std::pair{"gCullCommands", &myCullCommandBuffers.buffers},
				 std::pair{"gCullCounts", &myCullCountBuffers.buffers}})
			pipeline.SetDescriptorData(
				name, BufferBinding{.buffer = (*buffers)[frameIt], .offset = 0}, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES, frameIt);
	}
	pipeline.SetDescriptorData(
		"gTextures",
		ImageBinding{.sampler = {}, .imageView = myGraph.GetView(myDepth), .layout = ImageLayout::kShaderReadOnly},
		DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES,
		SHADER_TYPES_MAIN_DEPTH_TEXTURE);
	// a storage view per level of the depth pyramid, each of gDepthPyramid's slots past them the last level's (bound, so
	// none names a view of an earlier size's pyramid)
	{
		const auto& pyramid = myGraph.GetImage(myDepthPyramid);
		auto levelCount = std::min<size_t>(pyramid.GetDesc().mipLevels.size(), SHADER_TYPES_DEPTH_PYRAMID_LEVEL_COUNT);
		myDepthPyramidViews.clear();
		for (uint32_t levelIt = 0; levelIt < levelCount; levelIt++)
			myDepthPyramidViews.push_back(std::make_unique<ImageView>(ImageViewCreateDesc{
				device.CreateDeviceObjectCreateDesc(std::format("Depth Pyramid Level {}", levelIt)),
				pyramid,
				Format::kR32Sfloat,
				ImageAspect::kColor,
				1,
				{},
				levelIt}));
		for (uint32_t slotIt = 0; slotIt < SHADER_TYPES_DEPTH_PYRAMID_LEVEL_COUNT; slotIt++)
			pipeline.SetDescriptorData(
				"gDepthPyramid",
				ImageBinding{
					.sampler = {}, .imageView = *myDepthPyramidViews[std::min<size_t>(slotIt, levelCount - 1)], .layout = ImageLayout::kGeneral},
				DESCRIPTOR_SET_CATEGORY_GLOBAL_RW_TEXTURES,
				slotIt);
	}
	pipeline.SetDescriptorData(
		"gCullVisibility", BufferBinding{.buffer = myCullVisibilityBuffer, .offset = 0}, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES);
	// the depth reprojection's: the main depth's stencil, and the last frame's depth at this size (none yet)
	{
		const auto& depth = myGraph.GetImage(myDepth);
		myStencilView = std::make_unique<ImageView>(
			ImageViewCreateDesc{device.CreateDeviceObjectCreateDesc("Main Stencil View"), depth, depth.GetDesc().format, ImageAspect::kStencil});
		pipeline.SetDescriptorData(
			"gMainStencil",
			ImageBinding{.sampler = {}, .imageView = *myStencilView, .layout = ImageLayout::kShaderReadOnly},
			DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES);
		myPreviousDepthBuffer = Buffer(BufferCreateDesc{
			device.CreateDeviceObjectCreateDesc("Previous Depth"),
			renderer::PixelCount(extent) * sizeof(float),
			BufferUsage::kStorage,
			MemoryProperty::kDeviceLocal});
		pipeline.SetDescriptorData(
			"gPreviousDepth", BufferBinding{.buffer = myPreviousDepthBuffer, .offset = 0}, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES);
		pipeline.SetDescriptorData(
			"gReprojectedDepth", BufferBinding{.buffer = myGraph.GetBuffer(myReprojectedDepth), .offset = 0}, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES);
		myPreviousValid = false;
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

	// what the occlusion culling decided the last time this frame index ran (the caller waited for its fence), and
	// emptied for this frame
	{
		auto& buffer = myCullStatisticsBuffers[inputs.frameIndex];
		auto memory = buffer.Map();
		std::array<uint32_t, CULL_STATISTIC_COUNT> statistics{};
		buffer.Invalidate(0, sizeof(statistics));
		std::memcpy(statistics.data(), memory.data(), sizeof(statistics));
		if (std::ranges::any_of(statistics, [](uint32_t count) { return count != 0; }))
		{
			myCullStatistics.Write().Get() = statistics;
			if (myCollectTimings)
			{
				for (size_t statisticIt = 0; statisticIt < statistics.size(); statisticIt++)
					myCullStatisticSums[statisticIt] += statistics[statisticIt];
				myCullStatisticFrameCount++;
			}
		}
		std::memset(memory.data(), 0, sizeof(statistics));
		buffer.Flush(0, sizeof(statistics));
		buffer.Unmap();
	}

	FrameInputs frame = inputs;
	frame.pushConstants.framebufferWidth = myGraph.GetExtent().width;
	frame.pushConstants.oitNodeCapacity = myOitNodeCapacity;
	myFrame.inputs = &frame;
	myFrame.pipeline = &pipeline;

	// the scene's draws: in two phases if it has transmissive materials
	myTwoPhases = false;
	if (frame.model != nullptr)
	{
		const auto& materials = frame.model->GetDesc().materials;
		for (size_t materialIt = 0; materialIt < materials.size(); materialIt++)
			myTwoPhases |= !materials[materialIt].blend && frame.transmissive(materialIt);
	}
	for (auto phase : {MainPassPhase::kOpaque, MainPassPhase::kTransmissive})
		myDrawLists[std::to_underlying(phase)] = frame.model != nullptr
			? BuildDrawList(*frame.model, phase, myTwoPhases, frame.transmissive, frame.specialization)
			: DrawList{};
	// the shadows: planned for the frame, and written for the shader to read once the frame's previous use of its
	// buffers is done (the caller waited for its fence)
	ENSURE(frame.lights.size() <= SHADER_TYPES_LIGHT_COUNT); // see Scene::UpdateLights
	myShadowPlan = frame.shadows && frame.model != nullptr
		? shadows::Plan(frame.lights, frame.camera, frame.sceneBounds)
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
		auto lights = frame.lights;
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
		? BuildShadowDrawList(*frame.model, frame.transmissive)
		: DrawList{};
	bool shadowsDrawn = !myShadowDrawList.Empty();
	frame.pushConstants.shadows = shadowsDrawn ? 1U : 0U;
	frame.pushConstants.shadowView = SHADER_TYPES_NO_SHADOW;
	myGraph.SetEnabled(myShadowPass, shadowsDrawn);

	// the scattering materials' light, if any are drawn
	myScatterDrawList = frame.model != nullptr
		? BuildScatterDrawList(*frame.model, frame.scatters, frame.specialization)
		: DrawList{};
	myScatterBackDrawList = BuildScatterBackDrawList(myScatterDrawList);
	frame.pushConstants.scatter = myScatterDrawList.Empty() ? 0U : 1U;
	myGraph.SetEnabled(myScatterPass, !myScatterDrawList.Empty());
	myGraph.SetEnabled(myScatterBackPass, !myScatterDrawList.Empty());
	myGraph.SetEnabled(myScatterBlurPass, !myScatterDrawList.Empty());
	myGraph.SetEnabled(myScatterDenoisePass, !myScatterDrawList.Empty());

	// the draws' per draw values, in the frame's buffer (its previous use is done: after the frame's fence). a frame with
	// more draws than it holds gets a bigger one (NodePerformanceTest's two encodings side by side have 40000), and the
	// old one is kept until the frames in flight, whose descriptor sets name it, are done
	{
		myFrameCount++;
		std::erase_if(
			myRetiredBuffers, [this](const auto& retired) { return myFrameCount > retired.first + SHADER_TYPES_FRAME_COUNT; });
		std::array<DrawList*, std::to_underlying(ListId::kCount)> lists{};
		size_t count = 0;
		for (uint8_t listIt = 0; listIt < lists.size(); listIt++)
		{
			lists[listIt] = &InternalList(static_cast<ListId>(listIt));
			count += lists[listIt]->items.size();
			PlanDrawRuns(*lists[listIt], myDrawStreams[frame.frameIndex].GetMaxDrawCount());
		}
		auto& buffer = InternalReserve(myDrawRecords, pipeline, frame.frameIndex, count);
		std::vector<DrawRecord> records(count);
		WriteDrawRecords(lists, records);
		if (!records.empty())
		{
			auto bytes = std::as_bytes(std::span(records));
			auto memory = buffer.Map();
			std::ranges::copy(bytes, memory.begin());
			buffer.Flush(0, bytes.size());
			buffer.Unmap();
		}
	}
	// the ui's frosted panels: clipped to the frame, each a run of the UI Blur passes' threads, a pixel each (X's over
	// the blur's reach above and below too)
	{
		auto extent = myGraph.GetExtent();
		std::vector<UiBlurRect> rects;
		myUiBlurThreadsX = 0;
		myUiBlurThreadsY = 0;
		for (auto rect : frame.uiBlurRects)
		{
			ENSUREF(rects.size() < SHADER_TYPES_UI_BLUR_RECT_COUNT, "more than {} frosted ui panels (SHADER_TYPES_UI_BLUR_RECT_COUNT)", SHADER_TYPES_UI_BLUR_RECT_COUNT);
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

	// the depth reprojection's: the frame's height and main views, and the last frame's extent (see InternalPlanCulling)
	frame.pushConstants.framebufferHeight = myGraph.GetExtent().height;
	frame.pushConstants.mainViewCount = frame.grid.x * frame.grid.y;
	frame.pushConstants.previousWidth = myPreviousExtent.width;
	frame.pushConstants.previousHeight = myPreviousExtent.height;
	InternalPlanCulling(pipeline, frame);
	myGraph.Bind(myCullCommands, myCullCommandBuffers.buffers[frame.frameIndex]);
	myGraph.Bind(myCullCounts, myCullCountBuffers.buffers[frame.frameIndex]);
	myGraph.Bind(myCullVisibility, myCullVisibilityBuffer);
	myGraph.Bind(myPreviousDepth, myPreviousDepthBuffer);
	myGraph.Bind(myCullStatisticsId, myCullStatisticsBuffers[frame.frameIndex]);

	// the indirect draws' commands, written as the passes record them (the frame index's previous use of them is done)
	myDrawStream = &myDrawStreams[frame.frameIndex];
	// the whole frame's gpu time too, in the passes' timing group (where a backend doesn't time every pass, see
	// GpuProfiler::Collect, it still times the frame: its two timestamps are outside every pass)
	static constexpr SourceLocationData kFrameLocation = SOURCE_LOCATION_DATA(frame);
	GpuProfiler::Scope frameScope;
	if (auto* profiler = inputs.graphicsQueue->GetProfiler())
		frameScope = profiler->Begin(cmd, "Frame", kFrameLocation, myGraph.GetTimingGroup(), PipelineStage::kTopOfPipe);
	myGraph.Execute(cmd, inputs.graphicsQueue->GetProfiler());
	frameScope.End();
	myDrawStream->End();
	myDrawStream = nullptr;

	myFrame.inputs = nullptr;
	myFrame.pipeline = nullptr;
}

DrawList& Renderer::InternalList(ListId list) noexcept
{
	switch (list)
	{
	case ListId::kMain:
		return myDrawLists[0];
	case ListId::kMainTransmissive:
		return myDrawLists[1];
	case ListId::kShadow:
		return myShadowDrawList;
	case ListId::kScatter:
		return myScatterDrawList;
	case ListId::kScatterBack:
	case ListId::kCount:
		break;
	}
	return myScatterBackDrawList;
}

void Renderer::InternalCreate(GrowingBuffers& buffers, Device& device, size_t capacity)
{
	for (uint32_t frameIt = 0; frameIt < SHADER_TYPES_FRAME_COUNT; frameIt++)
	{
		buffers.buffers.emplace_back(BufferCreateDesc{
			device.CreateDeviceObjectCreateDesc(std::format("{} {}", buffers.name, frameIt)),
			capacity * buffers.elementSize,
			buffers.usage,
			buffers.memory});
		buffers.capacities.push_back(capacity);
	}
}

Buffer& Renderer::InternalReserve(GrowingBuffers& buffers, Pipeline& pipeline, uint16_t frameIndex, size_t count)
{
	if (count > buffers.capacities[frameIndex])
	{
		auto capacity = std::bit_ceil(count);
		auto& device = pipeline.GetDevice();
		Buffer grown(BufferCreateDesc{
			device.CreateDeviceObjectCreateDesc(std::format("{} {}", buffers.name, frameIndex)),
			capacity * buffers.elementSize,
			buffers.usage,
			buffers.memory});
		pipeline.SetDescriptorData(
			buffers.binding, BufferBinding{.buffer = grown, .offset = 0}, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES, frameIndex);
		myRetiredBuffers.emplace_back(myFrameCount, std::exchange(buffers.buffers[frameIndex], std::move(grown)));
		buffers.capacities[frameIndex] = capacity;
	}
	return buffers.buffers[frameIndex];
}

bool Renderer::InternalCullsOnGpu(ListId list, const FrameInputs& frame) const noexcept
{
	if (myGpuCulling)
		return true;
	// occlusion culls the main pass's lists on the gpu even where the count is emulated (see the constructor)
	return myOcclusionCulling && frame.culling && (list == ListId::kMain || list == ListId::kMainTransmissive);
}

namespace renderer
{

// a main view's viewport: its grid cell, letterboxed to its camera's aspect ratio (FrameInputs::viewports)
[[nodiscard]] Viewport ViewportOf(const FrameInputs& inputs, Extent2d extent, uint32_t view)
{
	uint32_t deltaX = extent.width / std::max(inputs.grid.x, 1U);
	uint32_t deltaY = extent.height / std::max(inputs.grid.y, 1U);
	auto x = static_cast<int32_t>((view % inputs.grid.x) * deltaX);
	auto y = static_cast<int32_t>((view / inputs.grid.x) * deltaY);
	auto width = deltaX;
	auto height = deltaY;
	if (view < inputs.viewports.size() && inputs.viewports[view].width > 0 && inputs.viewports[view].height > 0)
	{
		x = inputs.viewports[view].x;
		y = inputs.viewports[view].y;
		width = inputs.viewports[view].width;
		height = inputs.viewports[view].height;
	}
	return Viewport{
		.x = static_cast<float>(x),
		.y = static_cast<float>(y),
		.width = static_cast<float>(width),
		.height = static_cast<float>(height),
		.minDepth = 0.0F,
		.maxDepth = 1.0F};
}

} // namespace renderer

void Renderer::InternalPlanCulling(Pipeline& pipeline, const FrameInputs& frame)
{
	ZoneScopedN("Renderer::planCulling");

	using namespace renderer;

	myCullFirstJob.fill(kNoJob);
	myCullLateFirstJob.fill(kNoJob);
	myCullJobs.clear();
	myCullEarlyJobCount = 0;
	myCullCountCount = 0;
	myCullMaxCandidateCount = 0;
	myCullLateMaxCandidateCount = 0;
	const auto& mainList = InternalList(ListId::kMain);
	myOcclusionThisFrame = myOcclusionCulling && frame.culling && !mainList.Empty();
	auto disable = [this]
	{
		for (auto pass :
			 {myCullClearPass,
			  myCullPass,
			  myCullReprojectedPass,
			  myDepthReprojectClearPass,
			  myDepthReprojectPass,
			  myDepthPyramidReprojectedPass,
			  myDepthPyramidPass,
			  myCullLatePass,
			  myMainLatePass,
			  myDepthSavePass})
			myGraph.SetEnabled(pass, false);
		myPreviousValid = false;
	};
	bool anyOnGpu = false;
	for (uint8_t listIt = 0; listIt < std::to_underlying(ListId::kCount); listIt++)
		anyOnGpu |= InternalCullsOnGpu(static_cast<ListId>(listIt), frame) && !InternalList(static_cast<ListId>(listIt)).Empty();
	if (!anyOnGpu)
	{
		myOcclusionThisFrame = false;
		disable();
		return;
	}

	// the views: the main views', then the shadow views' frustums (all zeros, which culls nothing, without culling), and
	// the main views' view projections and viewports, for occlusion
	uint32_t mainViewCount = frame.grid.x * frame.grid.y;
	ENSURE(mainViewCount <= SHADER_TYPES_VIEW_COUNT);
	std::vector<CullView> views(mainViewCount + myShadowPlan.views.size());
	auto setView = [&views](size_t view, const glm::mat4& viewProjection)
	{
		auto frustum = Frustum::FromViewProjection(viewProjection);
		for (size_t planeIt = 0; planeIt < frustum.planes.size(); planeIt++)
			std::ranges::copy(std::span(glm::value_ptr(frustum.planes[planeIt]), 4), views[view].planes[planeIt]);
		std::ranges::copy(std::span(glm::value_ptr(viewProjection), 16), &views[view].viewProjection[0][0]);
	};
	if (frame.culling)
	{
		auto extent = myGraph.GetExtent();
		for (uint32_t viewIt = 0; viewIt < mainViewCount && viewIt < frame.viewProjections.size(); viewIt++)
		{
			setView(viewIt, frame.viewProjections[viewIt]);
			auto viewport = ViewportOf(frame, extent, viewIt);
			views[viewIt].viewport[0] = viewport.x;
			views[viewIt].viewport[1] = viewport.y;
			views[viewIt].viewport[2] = viewport.width;
			views[viewIt].viewport[3] = viewport.height;
			views[viewIt].occlusion = myOcclusionThisFrame ? 1U : 0U;
			if (myPreviousValid && myPreviousViewports.size() == mainViewCount)
			{
				const auto& previous = myPreviousViewports[viewIt];
				views[viewIt].previousValid = 1;
				std::ranges::copy(
					std::span(glm::value_ptr(myPreviousInverseViewProjections[viewIt]), 16), &views[viewIt].previousInverseViewProjection[0][0]);
				views[viewIt].previousViewport[0] = previous.x;
				views[viewIt].previousViewport[1] = previous.y;
				views[viewIt].previousViewport[2] = previous.width;
				views[viewIt].previousViewport[3] = previous.height;
			}
		}
		for (size_t viewIt = 0; viewIt < myShadowPlan.views.size(); viewIt++)
			setView(mainViewCount + viewIt, glm::make_mat4(&myShadowPlan.views[viewIt].viewProjection[0][0]));
	}

	// the candidates, list after list, and a job per list and view: the Cull pass's first, then the Cull Late pass's
	std::vector<CullCandidate> candidates;
	std::array<uint32_t, std::to_underlying(ListId::kCount)> candidateBases{};
	for (uint8_t listIt = 0; listIt < std::to_underlying(ListId::kCount); listIt++)
	{
		auto listId = static_cast<ListId>(listIt);
		const auto& list = InternalList(listId);
		candidateBases[listIt] = static_cast<uint32_t>(candidates.size());
		if (list.Empty() || !InternalCullsOnGpu(listId, frame))
			continue;
		for (uint32_t runIt = 0; runIt < list.runs.size(); runIt++)
		{
			const auto& run = list.runs[runIt];
			for (uint32_t itemIt = run.first; itemIt < run.first + run.count; itemIt++)
			{
				const auto& item = list.items[itemIt];
				const auto& min = item.bounds.GetMin();
				const auto& max = item.bounds.GetMax();
				candidates.push_back(CullCandidate{
					.boundsMin = {min.x, min.y, min.z, item.cullable ? 1.0F : 0.0F},
					.boundsMax = {max.x, max.y, max.z, 0.0F},
					.record = item.record,
					.indexCount = item.indexCount,
					.instanceCount = item.instanceCount,
					.firstIndex = item.firstIndex,
					.run = runIt,
					.runFirst = run.first,
					.late = item.variant.fragmentShader == kTransparentFragmentShader ? 1U : 0U});
			}
		}
	}

	uint32_t commandCount = 0;
	uint32_t visibilityCount = 0;
	// a list's jobs, a job per view, of the Cull pass (before the main pass) or the Cull Late pass (after it)
	auto addJobs = [&](ListId listId, uint32_t kind, uint32_t visibilityBase, bool late = false)
	{
		const auto& list = InternalList(listId);
		auto listIt = std::to_underlying(listId);
		bool shadow = listId == ListId::kShadow;
		auto firstView = shadow ? mainViewCount : 0U;
		auto viewCount = shadow ? static_cast<uint32_t>(myShadowPlan.views.size()) : mainViewCount;
		auto itemCount = static_cast<uint32_t>(list.items.size());
		(late ? myCullLateFirstJob : myCullFirstJob)[listIt] = static_cast<uint32_t>(myCullJobs.size());
		for (uint32_t viewIt = 0; viewIt < viewCount; viewIt++)
		{
			myCullJobs.push_back(CullJob{
				.candidateBase = candidateBases[listIt],
				.candidateCount = itemCount,
				.view = firstView + viewIt,
				.commandBase = commandCount,
				.countBase = myCullCountCount,
				.kind = kind,
				.visibilityBase = visibilityBase + viewIt * itemCount,
				.compact = myCountEmulated ? 0U : 1U});
			commandCount += itemCount;
			myCullCountCount += static_cast<uint32_t>(list.runs.size());
		}
		(late ? myCullLateMaxCandidateCount : myCullMaxCandidateCount) =
			std::max(late ? myCullLateMaxCandidateCount : myCullMaxCandidateCount, itemCount);
	};
	auto onGpu = [&](ListId listId) { return !InternalList(listId).Empty() && InternalCullsOnGpu(listId, frame); };
	for (uint8_t listIt = 0; listIt < std::to_underlying(ListId::kCount); listIt++)
	{
		auto listId = static_cast<ListId>(listIt);
		if (!onGpu(listId))
			continue;
		if (myOcclusionThisFrame && listId == ListId::kMain)
		{
			// the main pass's first draws: two-phase, those seen last frame, by their visibility (kept from frame to frame:
			// the main list's comes first, so its place stays as long as its draws and the views do); reprojected, all it
			// draws, by the last frame's depth; hybrid, those that doesn't hide (marked in the visibility)
			switch (myOcclusionMode)
			{
			case OcclusionMode::kTwoPhase: addJobs(listId, CULL_JOB_EARLY, visibilityCount); break;
			case OcclusionMode::kReprojected: addJobs(listId, CULL_JOB_PYRAMID, 0); break;
			case OcclusionMode::kHybrid: addJobs(listId, CULL_JOB_HYBRID_EARLY, visibilityCount); break;
			}
			continue;
		}
		if (myOcclusionThisFrame && listId == ListId::kMainTransmissive)
		{
			if (myOcclusionMode == OcclusionMode::kReprojected)
				addJobs(listId, CULL_JOB_PYRAMID, 0);
			continue; // else after the pyramid of the main pass's depth
		}
		addJobs(listId, CULL_JOB_FRUSTUM, 0);
	}
	myCullEarlyJobCount = static_cast<uint32_t>(myCullJobs.size());
	bool reprojected = myOcclusionThisFrame && myOcclusionMode != OcclusionMode::kTwoPhase;
	bool lateDraws = myOcclusionThisFrame && myOcclusionMode != OcclusionMode::kReprojected;
	if (lateDraws)
	{
		addJobs(ListId::kMain, myOcclusionMode == OcclusionMode::kHybrid ? CULL_JOB_HYBRID_LATE : CULL_JOB_LATE, visibilityCount, true);
		if (onGpu(ListId::kMainTransmissive))
			addJobs(ListId::kMainTransmissive, CULL_JOB_PYRAMID, 0, true);
	}
	if (myOcclusionThisFrame && myOcclusionMode != OcclusionMode::kReprojected)
		visibilityCount += static_cast<uint32_t>(mainList.items.size()) * mainViewCount;
	ENSURE(myCullJobs.size() <= kCullJobCount && views.size() <= kCullViewCount);

	// written for the gpu to read once the frame's previous use of them is done (the caller waited for its fence). the
	// commands and counts grow with them (NodePerformanceTest's two encodings: 40000 draws a view), and the visibility
	// (whose contents a bigger buffer loses: every draw then waits for the pyramid, once)
	auto write = [](Buffer& buffer, std::span<const std::byte> bytes)
	{
		if (bytes.empty())
			return;
		auto memory = buffer.Map();
		std::ranges::copy(bytes, memory.begin());
		buffer.Flush(0, bytes.size());
		buffer.Unmap();
	};
	write(InternalReserve(myCullCandidates, pipeline, frame.frameIndex, candidates.size()), std::as_bytes(std::span(candidates)));
	write(myCullJobBuffers[frame.frameIndex], std::as_bytes(std::span(myCullJobs)));
	write(myCullViewBuffers[frame.frameIndex], std::as_bytes(std::span(views)));
	InternalReserve(myCullCommandBuffers, pipeline, frame.frameIndex, commandCount);
	InternalReserve(myCullCountBuffers, pipeline, frame.frameIndex, myCullCountCount);
	if (visibilityCount > myCullVisibilityCapacity)
	{
		myCullVisibilityCapacity = std::bit_ceil(static_cast<size_t>(visibilityCount));
		auto& device = pipeline.GetDevice();
		Buffer grown(BufferCreateDesc{
			device.CreateDeviceObjectCreateDesc("Cull Visibility"),
			myCullVisibilityCapacity * sizeof(uint32_t),
			BufferUsage::kStorage,
			MemoryProperty::kDeviceLocal});
		pipeline.SetDescriptorData("gCullVisibility", BufferBinding{.buffer = grown, .offset = 0}, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES);
		myRetiredBuffers.emplace_back(myFrameCount, std::exchange(myCullVisibilityBuffer, std::move(grown)));
	}

	myGraph.SetEnabled(myCullClearPass, myCullCountCount > 0 && !myCountEmulated);
	myGraph.SetEnabled(myCullPass, myCullEarlyJobCount > 0 && !reprojected);
	myGraph.SetEnabled(myCullReprojectedPass, myCullEarlyJobCount > 0 && reprojected);
	myGraph.SetEnabled(myDepthReprojectClearPass, reprojected);
	myGraph.SetEnabled(myDepthReprojectPass, reprojected && myPreviousValid);
	myGraph.SetEnabled(myDepthPyramidReprojectedPass, reprojected);
	myGraph.SetEnabled(myDepthPyramidPass, lateDraws);
	myGraph.SetEnabled(myCullLatePass, lateDraws);
	myGraph.SetEnabled(myMainLatePass, lateDraws);
	myGraph.SetEnabled(myDepthSavePass, reprojected);

	// what the next frame reprojects: this frame's depth (once the Depth Save pass has run), from its views
	myPreviousValid = reprojected && frame.viewProjections.size() >= mainViewCount;
	myPreviousExtent = myGraph.GetExtent();
	myPreviousInverseViewProjections.clear();
	myPreviousViewports.clear();
	if (myPreviousValid)
		for (uint32_t viewIt = 0; viewIt < mainViewCount; viewIt++)
		{
			myPreviousInverseViewProjections.push_back(glm::inverse(frame.viewProjections[viewIt]));
			myPreviousViewports.push_back(ViewportOf(frame, myPreviousExtent, viewIt));
		}
}

std::optional<CulledDraws> Renderer::InternalCulledDraws(ListId list, uint32_t view, bool late) const
{
	// a list drawn by one pass has its jobs in one of the sets: the transmissive phase's are the Cull Late pass's, but for
	// one phase occlusion (after the pyramid of the main pass's depth)
	auto listIt = std::to_underlying(list);
	auto firstJob = late ? myCullLateFirstJob[listIt]
						 : (myCullFirstJob[listIt] != kNoJob || list != ListId::kMainTransmissive ? myCullFirstJob[listIt]
																								  : myCullLateFirstJob[listIt]);
	if (firstJob == kNoJob)
	{
		// the gpu culls it, so the cpu mustn't draw it as well
		ENSUREF(InternalList(list).Empty() || !InternalCullsOnGpu(list, *myFrame.inputs), "no cull job for a list the gpu culls");
		return std::nullopt;
	}
	ENSURE(firstJob + view < myCullJobs.size());
	const auto& job = myCullJobs[firstJob + view];
	auto frameIndex = myFrame.inputs->frameIndex;
	return CulledDraws{
		.commands = &myCullCommandBuffers.buffers[frameIndex],
		.counts = &myCullCountBuffers.buffers[frameIndex],
		.commandBase = job.commandBase,
		.countBase = job.countBase,
		.counted = job.compact != 0};
}

void Renderer::InternalDrawViews(FrameGraph::PassContext& context, Pipeline& pipeline, const FrameInputs& inputs, ListId listId, bool late)
{
	const auto& list = InternalList(listId);
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
	ASSERT(extent.width >= grid.x && extent.height >= grid.y);

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

		auto viewIt = static_cast<uint16_t>(drawIt);
		auto viewport = renderer::ViewportOf(inputs, extent, viewIt);
		encoder.SetViewport(viewport);
		encoder.SetScissor(rhi::Rect{
			.x = static_cast<int32_t>(viewport.x),
			.y = static_cast<int32_t>(viewport.y),
			.width = static_cast<uint32_t>(viewport.width),
			.height = static_cast<uint32_t>(viewport.height)});

		if (auto culled = InternalCulledDraws(listId, viewIt, late))
		{
			RecordCulledDrawList(cmd, pipeline, list, inputs.pushConstants, viewIt, *culled, inputs.executor);
			continue;
		}
		std::optional<Frustum> frustum;
		if (inputs.culling && viewIt < inputs.viewProjections.size())
			frustum = Frustum::FromViewProjection(inputs.viewProjections[viewIt]);
		RecordDrawList(cmd, pipeline, list, inputs.pushConstants, viewIt, *myDrawStream, frustum ? &*frustum : nullptr, inputs.executor);
	}

	std::vector<CommandBuffer> secondaries;
	secondaries.push_back(std::move(cmd));
	CommandEncoder(context.cmd).ExecuteCommands(std::move(secondaries));
}

} // namespace gfx
