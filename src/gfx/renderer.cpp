#include "renderer.h"

#include <gfx/model.h>

#include <core/assert.h>
#include <core/file.h>
#include <core/profiling.h>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <bit>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <numeric>
#include <optional>
#include <print>
#include <ranges>
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
// the indirect lighting's kernels (see gi.slang), entry points 9 to 17
constexpr uint8_t kGiShiftEntryPoint = 9;
constexpr uint8_t kGiVoxelizeEntryPoint = 10;
constexpr uint8_t kGiLightStaticEntryPoint = 11;
constexpr uint8_t kGiLightEntryPoint = 12;
constexpr uint8_t kGiFlagsEntryPoint = 13;
constexpr uint8_t kGiTraceEntryPoint = 14;
constexpr uint8_t kGiProbeUpdateEntryPoint = 15;
constexpr uint8_t kGiResolveEntryPoint = 16;
constexpr uint8_t kGiRestoreEntryPoint = 17;
// the traced shadows' (see tracedshadows.slang)
constexpr uint8_t kShadowClassifyEntryPoint = 18;
constexpr uint8_t kShadowTraceEntryPoint = 19;
constexpr uint8_t kShadowDenoiseXEntryPoint = 20;
constexpr uint8_t kShadowDenoiseYEntryPoint = 21;
// the surface cache's pooling (see ComputeGiCacheFilter), after the resolve
constexpr uint8_t kGiCacheFilterEntryPoint = 22;
// and its storage's, before the resolve (see ComputeGiCachePrepare, ComputeGiCacheRelease)
constexpr uint8_t kGiCachePrepareEntryPoint = 23;
constexpr uint8_t kGiCacheReleaseEntryPoint = 24;
// the near field's (see ginear.slang): its rays, and their filter along rows and columns
constexpr uint8_t kGiNearEntryPoint = 25;
constexpr uint8_t kGiNearXEntryPoint = 26;
constexpr uint8_t kGiNearYEntryPoint = 27;
constexpr uint8_t kGiNearTemporalEntryPoint = 28;
// the emitters' (see emitters.slang): the reservoirs, their reuse and shadow rays, the filter and the frames
constexpr uint8_t kEmitterSampleEntryPoint = 29;
constexpr uint8_t kEmitterShadeEntryPoint = 30;
constexpr uint8_t kEmitterXEntryPoint = 31;
constexpr uint8_t kEmitterYEntryPoint = 32;
constexpr uint8_t kEmitterTemporalEntryPoint = 33;
// the bytes of a page of the cache (a block's 64 entries of 3 uint4: see GiCacheEntry), and the pages and records its
// pools grow by at least
constexpr uint64_t kGiCachePageSize = 64 * 3 * 4 * sizeof(uint32_t);
constexpr uint32_t kGiCachePageStep = 1024;
// the frames after a reset of the indirect lighting before its voxels take the probes' light (see InternalPlanGi)
constexpr uint32_t kGiBounceDelayFrameCount = 32;
// the frames after lights last changed gradually (see Renderer::myGiLightsGradual) for which the surface cache samples
// as where something moves, so that it follows them: about a second
constexpr uint32_t kGiLightsFollowFrameCount = 64;

// at most: the main list's early and late jobs, the transmissive and the scatter lists' per main view, and the shadow
// list's early and late per shadow view
constexpr uint32_t kCullJobCount = (5 * SHADER_TYPES_VIEW_COUNT) + (2 * SHADER_TYPES_SHADOW_VIEW_COUNT);
constexpr uint32_t kCullViewCount = SHADER_TYPES_VIEW_COUNT + SHADER_TYPES_SHADOW_VIEW_COUNT;

} // namespace renderer

Renderer::Renderer(Device& device, RenderOutput& output)
	: myRenderOutput(output)
{
	using namespace renderer;

	// linear: shading and blending happen in linear space, and ComputeMain applies the srgb curve when it copies the
	// result to the swapchain
	myColor = myGraph.CreateImage({.name = "Main Color", .format = Format::kR16G16B16A16Sfloat});
	myPrepassNormal = myGraph.CreateImage({.name = "Prepass Normal", .format = Format::kR8G8B8A8Unorm});
	myShadowRaw = myGraph.CreateImage({.name = "Shadow Raw", .format = Format::kR8G8B8A8Unorm});
	myShadowBlocker = myGraph.CreateImage({.name = "Shadow Blocker", .format = Format::kR16G16B16A16Sfloat});
	myShadowRows = myGraph.CreateImage({.name = "Shadow Rows", .format = Format::kR8G8B8A8Unorm});
	myShadowMask = myGraph.CreateImage({.name = "Shadow Mask", .format = Format::kR8G8B8A8Unorm});
	// the indirect lighting's near field (see SHADER_TYPES_GI_NEAR_RAW_TEXTURE)
	myGiNearRaw = myGraph.CreateImage({.name = "GI Near Raw", .format = Format::kR16G16B16A16Sfloat});
	myGiNearRows = myGraph.CreateImage({.name = "GI Near Rows", .format = Format::kR16G16B16A16Sfloat});
	myGiNear = myGraph.CreateImage({.name = "GI Near", .format = Format::kR16G16B16A16Sfloat});
	for (uint32_t historyIt = 0; historyIt < 2; historyIt++)
	{
		myGiNearHistory[historyIt] = myGraph.ImportImage(std::format("GI Near History {}", historyIt));
		myGiNearHistoryDistance[historyIt] = myGraph.ImportImage(std::format("GI Near History Distance {}", historyIt));
	}
	// emissive surfaces as lights (see SHADER_TYPES_EMITTER_RAW_TEXTURE): a texel per traced pixel (every stride-th each way,
	// SPEEDO_EMITTER_STRIDE: 1, 2, 4 or 8): its light, filtered along rows and columns, and over frames with its surface;
	// and the reservoirs (see Resize)
	if (const char* stride = std::getenv("SPEEDO_EMITTER_STRIDE"); stride != nullptr && *stride != '\0')
		myEmitterStride = std::bit_floor(std::clamp<uint32_t>(static_cast<uint32_t>(std::strtoul(stride, nullptr, 10)), 1U, 8U));
	myEmitterRaw = myGraph.CreateImage({.name = "Emitter Raw", .format = Format::kR16G16B16A16Sfloat, .downscale = myEmitterStride});
	myEmitterRows = myGraph.CreateImage({.name = "Emitter Rows", .format = Format::kR16G16B16A16Sfloat, .downscale = myEmitterStride});
	myEmitterFiltered = myGraph.CreateImage({.name = "Emitter Filtered", .format = Format::kR16G16B16A16Sfloat, .downscale = myEmitterStride});
	for (uint32_t historyIt = 0; historyIt < 2; historyIt++)
	{
		myEmitterHistory[historyIt] = myGraph.ImportImage(std::format("Emitter History {}", historyIt));
		myEmitterSurfaces[historyIt] = myGraph.ImportImage(std::format("Emitter Surface {}", historyIt));
	}
	myEmitterReservoirs = myGraph.ImportBuffer("Emitter Reservoirs");
	// the pixels whose shadows the atlas isn't sure of, for the rays (gShadowTraceList: a count, then a pixel each)
	myShadowTraceList = myGraph.CreateBuffer(
		{.name = "Shadow Trace List", .sizeOf = [](Extent2d extent) { return (PixelCount(extent) + 1) * sizeof(uint32_t); }});
	// and the 8x8 tiles within the filter's reach of them (gShadowTraceTiles), which the Shadow Denoise filters
	myShadowTraceTiles = myGraph.CreateBuffer(
		{.name = "Shadow Trace Tiles",
		 .sizeOf = [](Extent2d extent)
		 { return static_cast<uint64_t>((extent.width + 7) / 8) * ((extent.height + 7) / 8) * sizeof(uint32_t); }});
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
		myShadowTraceBuffers.emplace_back(BufferCreateDesc{
			device.CreateDeviceObjectCreateDesc(std::format("Shadow Trace {}", frameIt)),
			sizeof(ShadowTraceData),
			BufferUsage::kStorage,
			MemoryProperty::kHostVisible});
		myEmitterDataBuffers.emplace_back(BufferCreateDesc{
			device.CreateDeviceObjectCreateDesc(std::format("Emitter Data {}", frameIt)),
			sizeof(EmitterData),
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
			CULL_STATISTIC_COUNT * CULL_STATISTIC_GROUP_COUNT * sizeof(uint32_t),
			BufferUsage::kStorage,
			MemoryProperty::kHostVisible});
		myGiStatisticsBuffers.emplace_back(BufferCreateDesc{
			device.CreateDeviceObjectCreateDesc(std::format("GI Statistics {}", frameIt)),
			(GI_STATISTIC_COUNT + SHADER_TYPES_GI_CLIP_COUNT * GI_DUMP_FLOAT_COUNT) * sizeof(uint32_t),
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
	myGiStatisticsId = myGraph.ImportBuffer("GI Statistics");
	// the indirect lighting's (see InternalPlanGi): the persistent buffers, a texel each until a model is voxelized
	myGiVoxels = myGraph.ImportBuffer("GI Voxels");
	myGiRadiance = myGraph.ImportBuffer("GI Radiance");
	myGiRays = myGraph.ImportBuffer("GI Rays");
	myGiIrradiance = myGraph.ImportBuffer("GI Irradiance");
	myGiProbeDepth = myGraph.ImportBuffer("GI Probe Depth");
	myGiFlags = myGraph.ImportBuffer("GI Flags");
	myGiVisibility = myGraph.ImportBuffer("GI Visibility");
	myGiCache = myGraph.ImportBuffer("GI Cache");
	myGiCacheRecords = myGraph.ImportBuffer("GI Cache Records");
	myGiCacheTables = myGraph.ImportBuffer("GI Cache Tables");
	myGiCacheFree = myGraph.ImportBuffer("GI Cache Free");
	for (uint32_t volumeIt = 0; volumeIt < SHADER_TYPES_GI_VOLUME_COUNT; volumeIt++)
		myGiVolumes[volumeIt] = myGraph.ImportImage(std::format("GI Volume {}", volumeIt));
	myGiStaticVoxels = myGraph.ImportBuffer("GI Static Voxels");
	for (auto [buffer, name, size] : std::array{
			 std::tuple{&myGiVoxelBuffer, "GI Voxels", sizeof(GiVoxel)},
			 std::tuple{&myGiStaticVoxelBuffer, "GI Static Voxels", sizeof(GiVoxel)},
			 std::tuple{&myGiRadianceBuffer, "GI Radiance", 2 * sizeof(uint32_t)},
			 std::tuple{&myGiRayBuffer, "GI Rays", 2 * sizeof(uint32_t)},
			 std::tuple{&myGiIrradianceBuffer, "GI Irradiance", SHADER_TYPES_GI_IRRADIANCE_COUNT * 4 * sizeof(float)},
			 std::tuple{&myGiProbeDepthBuffer, "GI Probe Depth", sizeof(uint32_t)},
			 std::tuple{&myGiFlagBuffer, "GI Flags", sizeof(uint32_t)},
			 std::tuple{&myGiVisibilityBuffer, "GI Visibility", sizeof(uint32_t)},
			 std::tuple{&myGiCacheBuffer, "GI Cache", 3 * 4 * sizeof(uint32_t)},
			 std::tuple{&myGiCacheRecordBuffer, "GI Cache Records", 3 * 4 * sizeof(uint32_t)},
			 std::tuple{&myGiCacheTableBuffer, "GI Cache Tables", sizeof(uint32_t)},
			 std::tuple{&myGiCacheFreeBuffer, "GI Cache Free", 2 * sizeof(uint32_t)}})
		*buffer = Buffer(BufferCreateDesc{
			device.CreateDeviceObjectCreateDesc(name),
			size,
			BufferUsage::kStorage | BufferUsage::kTransferSource | BufferUsage::kTransferDestination,
			MemoryProperty::kDeviceLocal});
	for (uint32_t frameIt = 0; frameIt < SHADER_TYPES_FRAME_COUNT; frameIt++)
		myGiDataBuffers.emplace_back(BufferCreateDesc{
			device.CreateDeviceObjectCreateDesc(std::format("GI Data {}", frameIt)),
			SHADER_TYPES_GI_CLIP_COUNT * sizeof(GiData),
			BufferUsage::kStorage,
			MemoryProperty::kHostVisible});
	myGiJobs = {.name = "GI Jobs", .binding = "gGiJobs", .elementSize = 4 * sizeof(uint32_t)};
	InternalCreate(myGiJobs, device, 1024);
	myGiLightCells = {.name = "GI Light Cells", .binding = "gGiLightCells", .elementSize = 2 * sizeof(uint32_t)};
	InternalCreate(myGiLightCells, device, SHADER_TYPES_GI_LIGHT_GRID * SHADER_TYPES_GI_LIGHT_GRID * SHADER_TYPES_GI_LIGHT_GRID);
	myGiLightIndices = {.name = "GI Light Indices", .binding = "gGiLightIndices", .elementSize = sizeof(uint32_t)};
	InternalCreate(myGiLightIndices, device, 4096);
	myCullVisibilityCapacity = SHADER_TYPES_DRAW_RECORD_COUNT;
	myCullVisibilityBuffer = Buffer(BufferCreateDesc{
		device.CreateDeviceObjectCreateDesc("Cull Visibility"),
		myCullVisibilityCapacity * sizeof(uint32_t),
		BufferUsage::kStorage,
		MemoryProperty::kDeviceLocal});
	// the main depth's farthest per texel, by level (see ComputeDepthPyramid), which occlusion culling tests against
	myDepthPyramid = myGraph.CreateImage({.name = "Depth Pyramid", .format = Format::kR32Sfloat, .mipChain = true, .downscale = 2});
	myShadowDepthPyramid = myGraph.CreateImage(
		{.name = "Shadow Depth Pyramid",
		 .format = Format::kR32Sfloat,
		 .extent = {.width = shadows::kAtlasSize / 2, .height = shadows::kAtlasSize / 2},
		 .mipChain = true});
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
					  uint32_t depthPyramidSource = 0,
					  uint32_t giStep = 0,
					  uint32_t giClip = 0)
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
		pushConstants.giStep = giStep;
		pushConstants.giClip = giClip;
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
		// source 2: the shadow atlas, into the shadow views' pyramid (see ComputeDepthPyramid)
		const auto& levels = context.graph.GetImage(source == 2 ? myShadowDepthPyramid : myDepthPyramid).GetDesc().mipLevels;
		auto levelCount = (source == 2 ? myShadowDepthPyramidViews : myDepthPyramidViews).size();
		for (uint32_t levelIt = 0; levelIt < levelCount; levelIt++)
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
	auto shadowDraws = [this](FrameGraph::PassContext& context, bool late)
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
			if (auto culled = InternalCulledDraws(ListId::kShadow, viewIt, late))
			{
				RecordCulledDrawList(cmd, pipeline, myShadowDrawList, pushConstants, 0, *culled);
				continue;
			}
			auto frustum = Frustum::FromViewProjection(glm::make_mat4(&myShadowPlan.views[viewIt].viewProjection[0][0]));
			RecordDrawList(cmd, pipeline, myShadowDrawList, pushConstants, 0, *myDrawStream, inputs.culling ? &frustum : nullptr);
		}
	};
	myShadowPass = myGraph.AddPass(
		"Shadows",
		[this, useCulled](FrameGraph::PassBuilder& builder)
		{
			builder.DepthAttachment(myShadowAtlas, LoadOp::kClear);
			useCulled(builder);
		},
		[shadowDraws](FrameGraph::PassContext& context) { shadowDraws(context, false); });

	// the shadow views' occlusion culling: what the shadow views saw last frame drew first, then the rest that a pyramid
	// of the atlas doesn't hide
	myShadowDepthPyramidPass = myGraph.AddPass(
		"Shadow Depth Pyramid",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myShadowAtlas, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myShadowDepthPyramid, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderWrite));
		},
		[pyramid](FrameGraph::PassContext& context) { pyramid(context, 2); });
	myCullShadowsLatePass = myGraph.AddPass(
		"Cull Shadows Late",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myCullCounts, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite));
			builder.Use(myCullCommands, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderWrite));
			builder.Use(myCullStatisticsId, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite));
			builder.Use(myCullVisibility, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite));
			builder.Use(myShadowDepthPyramid, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead));
		},
		[this, kernel](FrameGraph::PassContext& context)
		{
			kernel(
				context.cmd,
				ComputePipelineVariant{.entryPoint = kCullEntryPoint},
				myCullShadowLateMaxCandidateCount,
				myCullShadowLateJobCount,
				myCullShadowLateJobBase);
		});
	myShadowsLatePass = myGraph.AddPass(
		"Shadows Late",
		[this, useCulled](FrameGraph::PassBuilder& builder)
		{
			builder.DepthAttachment(myShadowAtlas, LoadOp::kLoad);
			useCulled(builder);
		},
		[shadowDraws](FrameGraph::PassContext& context) { shadowDraws(context, true); });

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

	// the pre-pass: the main list's opaque draws' depth and geometry normals (the first phase, the draws seen last frame,
	// then the late ones, as the main pass's were: the occlusion culling's depth is the pre-pass's), which the main pass
	// then shades over, each pixel once (DepthTest::kEqual), and the traced shadows start from
	auto prepass = [&](bool late)
	{
		auto load = late ? LoadOp::kLoad : LoadOp::kClear;
		return myGraph.AddPass(
			late ? "Prepass Late" : "Prepass",
			[this, load, useCulled](FrameGraph::PassBuilder& builder)
			{
				useCulled(builder);
				builder.ColorAttachment(myPrepassNormal, load, StoreOp::kStore, ClearValue{.color = {0.5F, 0.5F, 1.0F, 0.0F}});
				builder.DepthAttachment(myDepth, load);
				builder.Contents(SubpassContents::kSecondaryCommandBuffers);
			},
			[this, late](FrameGraph::PassContext& context)
			{ InternalDrawViews(context, *myFrame.pipeline, *myFrame.inputs, ListId::kMain, late, true); });
	};
	// the main pass: the views, each in its viewport, over the pre-pass's depth. alpha 0 where nothing opaque is drawn
	// (opaque draws write alpha 1): there ComputeMain draws the environment
	auto mainPass = [&](MainPassPhase phase, bool late = false)
	{
		auto load = phase == MainPassPhase::kOpaque && !late ? LoadOp::kClear : LoadOp::kLoad;
		return myGraph.AddPass(
			late ? "Main Late" : (phase == MainPassPhase::kOpaque ? "Main" : "Main Transmissive"),
			[this, load, phase, &useOitLists, useCulled](FrameGraph::PassBuilder& builder)
			{
				useCulled(builder);
				// the indirect lighting's probes, and the voxels (where they are inside something)
				builder.Use(myGiIrradiance, ResourceUse::Storage(PipelineStage::kFragmentShader, Access::kShaderRead));
				builder.Use(myGiVoxels, ResourceUse::Storage(PipelineStage::kFragmentShader, Access::kShaderRead));
				// sampled in the general layout the resolve writes them in (see gGiVolumes)
				for (auto volume : myGiVolumes)
					builder.Use(volume, ResourceUse::Storage(PipelineStage::kFragmentShader, Access::kShaderRead));
				builder.ColorAttachment(myColor, load, StoreOp::kStore, ClearValue{.color = {0.0F, 0.0F, 0.0F, 0.0F}});
				builder.DepthAttachment(myDepth, LoadOp::kLoad);
				useOitLists(builder, PipelineStage::kFragmentShader, kOitAccess);
				builder.Use(myShadowAtlas, ResourceUse::Sampled(PipelineStage::kFragmentShader));
				if (phase == MainPassPhase::kTransmissive)
					builder.Use(myTransmission, ResourceUse::Sampled(PipelineStage::kFragmentShader));
				builder.Use(myScatterLight, ResourceUse::Sampled(PipelineStage::kFragmentShader));
				builder.Use(myShadowMask, ResourceUse::Sampled(PipelineStage::kFragmentShader));
				builder.Use(myGiNear, ResourceUse::Sampled(PipelineStage::kFragmentShader));
				for (auto image : {myGiNearHistory[0], myGiNearHistory[1], myGiNearHistoryDistance[0], myGiNearHistoryDistance[1]})
					builder.Use(image, ResourceUse::Storage(PipelineStage::kFragmentShader, Access::kShaderRead));
				for (auto image : {myEmitterHistory[0], myEmitterHistory[1], myEmitterSurfaces[0], myEmitterSurfaces[1]})
					builder.Use(image, ResourceUse::Storage(PipelineStage::kFragmentShader, Access::kShaderRead));
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
	// the indirect lighting (see gi.slang): the voxels (again when the model changes), lit, their levels, the probes'
	// rays through them, blended into the probes, and the probes resolved per voxel, which the main pass shades with.
	// each kernel's dispatches in a pass read what the one before wrote: barriers between them
	// a gi kernel over count threads, in rows (see GiThread)
	auto giKernel = [kernel](CommandBufferHandle cmd, uint8_t entryPoint, uint64_t count, uint32_t clip, uint32_t step = 0, uint32_t jobBase = 0)
	{
		constexpr uint64_t kRow = SHADER_TYPES_GI_ROW;
		auto rows = (count + kRow - 1) / kRow;
		ENSUREF(rows <= 65535, "the indirect lighting's {} threads are more than a dispatch takes", count);
		kernel(
			cmd,
			ComputePipelineVariant{.entryPoint = entryPoint},
			static_cast<uint32_t>(std::min(count, kRow)),
			static_cast<uint32_t>(std::max<uint64_t>(rows, 1)),
			jobBase,
			0,
			0,
			step,
			clip);
	};
	auto computeBarrier = [](CommandBufferHandle cmd)
	{
		CommandEncoder(cmd).Barrier(
			PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite, PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite);
	};
	// per clip (see gi::MakeClipmap): those moved or new are voxelized, every one is lit and its probes updated
	auto clips = [this](auto&& fn)
	{
		for (uint32_t clipIt = 0; clipIt < myGiClipmap.clips.size(); clipIt++)
			fn(clipIt, myGiClipmap.clips[clipIt]);
	};
	// the buffers made for a clipmap hold whatever their memory held before: zero before anything reads them (the
	// kernels guard against what isn't finite, but a finite value that is wrong is light; made over Bistro's, the
	// volumes of the square were cyan and red for a while)
	myGiClearPass = myGraph.AddPass(
		"GI Clear",
		[this](FrameGraph::PassBuilder& builder)
		{
			for (auto buffer : {myGiVoxels, myGiStaticVoxels, myGiRadiance, myGiRays, myGiIrradiance, myGiProbeDepth, myGiFlags, myGiVisibility, myGiCache})
				builder.Use(buffer, ResourceUse::TransferDestination());
			// the volumes too: the resolve writes only the blocks near a surface, and a lookup's filter reaches into the
			// ones beside them
			for (auto volume : myGiVolumes)
				builder.Use(volume, ResourceUse::TransferDestination());
		},
		[this](FrameGraph::PassContext& context)
		{
			CommandEncoder encoder(context.cmd);
			for (auto buffer : {myGiVoxels, myGiStaticVoxels, myGiRadiance, myGiRays, myGiIrradiance, myGiProbeDepth, myGiFlags, myGiVisibility, myGiCache})
				encoder.FillBuffer(context.graph.GetBuffer(buffer), 0, 0, 0);
			for (auto volume : myGiVolumes)
				context.graph.GetImage(volume).Clear(context.cmd, ClearValue{.color = {0.0F, 0.0F, 0.0F, 0.0F}});
		});
	// a clip voxelized starts from the static voxels kept, shifted by how far it moved (none of them when all of it is
	// voxelized again), so that only what scrolled in is voxelized
	myGiShiftPass = myGraph.AddPass(
		"GI Shift",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myGiStaticVoxels, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead));
			builder.Use(myGiVoxels, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderWrite));
		},
		[this, giKernel, clips](FrameGraph::PassContext& context)
		{
			clips(
				[&](uint32_t clip, const gi::Layout& layout)
				{
					if (myGiClipJobs[clip].voxelize)
						giKernel(context.cmd, kGiShiftEntryPoint, layout.voxelCount, clip);
				});
		});
	myGiVoxelizePass = myGraph.AddPass(
		"GI Voxelize",
		[this](FrameGraph::PassBuilder& builder)
		{ builder.Use(myGiVoxels, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite)); },
		[this, giKernel, clips](FrameGraph::PassContext& context)
		{
			clips(
				[&](uint32_t clip, const gi::Layout&)
				{
					const auto& jobs = myGiClipJobs[clip];
					if (jobs.voxelize && jobs.count > 0)
						giKernel(context.cmd, kGiVoxelizeEntryPoint, jobs.triangleCount, clip, jobs.count, jobs.base);
				});
		});
	// the static voxels kept, and each frame what moves voxelized over a copy of them
	myGiKeepPass = myGraph.AddPass(
		"GI Keep",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myGiVoxels, ResourceUse::TransferSource());
			builder.Use(myGiStaticVoxels, ResourceUse::TransferDestination());
		},
		[this, clips](FrameGraph::PassContext& context)
		{
			// the clips voxelized this frame only: the others' voxels hold what moved last frame
			clips(
				[&](uint32_t clip, const gi::Layout& layout)
				{
					if (myGiClipJobs[clip].voxelize)
						CommandEncoder(context.cmd).CopyBuffer(
							myGiVoxelBuffer, myGiStaticVoxelBuffer, layout.voxelCount * sizeof(GiVoxel), layout.data.voxelFirst * sizeof(GiVoxel));
				});
		});
	// the static voxels' direct light, kept (GiData::directFirst): when a clip is voxelized, or the lights change
	myGiLightStaticPass = myGraph.AddPass(
		"GI Light Static",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myGiVoxels, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead));
			builder.Use(myGiRadiance, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderWrite));
		},
		[this, giKernel, clips](FrameGraph::PassContext& context)
		{
			clips(
				[&](uint32_t clip, const gi::Layout& layout)
				{
					if (myGiClipJobs[clip].voxelize || myGiLightsChanged)
						giKernel(context.cmd, kGiLightStaticEntryPoint, layout.voxelCount, clip);
				});
		});
	myGiRestorePass = myGraph.AddPass(
		"GI Restore",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myGiStaticVoxels, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead));
			builder.Use(myGiVoxels, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderWrite));
			builder.Use(myGiFlags, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead));
		},
		[this, giKernel, clips](FrameGraph::PassContext& context)
		{
			// the blocks something moving was in last frame only
			clips([&](uint32_t clip, const gi::Layout& layout) { giKernel(context.cmd, kGiRestoreEntryPoint, layout.flagCount, clip); });
		});
	myGiVoxelizeMovingPass = myGraph.AddPass(
		"GI Voxelize Moving",
		[this](FrameGraph::PassBuilder& builder)
		{ builder.Use(myGiVoxels, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite)); },
		[this, giKernel, clips](FrameGraph::PassContext& context)
		{
			clips(
				[&](uint32_t clip, const gi::Layout&)
				{ giKernel(context.cmd, kGiVoxelizeEntryPoint, myGiMovingTriangleCount, clip, myGiMovingJobCount, myGiMovingJobBase); });
		});
	// the voxels lit (by the lights, and by the probes' light of the last frame off them: the light bounces on, a bounce
	// a frame), their levels, the probes' rays, the probes blended from them, and each voxel's probes, for the shading and
	// the next frame's bounce. every clip is lit and has its levels before any is traced (rays go on into the coarser
	// clips), and every clip's probes are updated before any is resolved
	// the cache's pool grown (see InternalPlanGi): the pages it had copied into the new one, and its free list, which the
	// GI pass then adds the new pages to (ComputeGiCachePrepare)
	myGiCacheGrowPass = myGraph.AddPass(
		"GI Cache Grow",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myGiCache, ResourceUse::TransferDestination());
			builder.Use(myGiCacheFree, ResourceUse::TransferDestination());
		},
		[this](FrameGraph::PassContext& context)
		{
			CommandEncoder encoder(context.cmd);
			// what the frames before wrote to the old buffers, under the ids the new ones are bound to now: the graph
			// tracks the ids, so their writes aren't made visible to these reads but here
			encoder.Barrier(PipelineStage::kComputeShader, Access::kShaderWrite, PipelineStage::kTransfer, Access::kTransferRead);
			encoder.CopyBuffer(myGiCacheGrowPages, context.graph.GetBuffer(myGiCache), myGiCacheGrowFirst * kGiCachePageSize);
			encoder.CopyBuffer(myGiCacheGrowFree, context.graph.GetBuffer(myGiCacheFree), (2 + static_cast<uint64_t>(myGiCacheGrowFirst)) * sizeof(uint32_t));
		});
	myGiPass = myGraph.AddPass(
		"GI",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myGiVoxels, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead));
			builder.Use(myGiRadiance, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite));
			builder.Use(myGiRays, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite));
			builder.Use(myGiIrradiance, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite));
			builder.Use(myGiProbeDepth, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite));
			builder.Use(myGiFlags, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite));
			builder.Use(myGiVisibility, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite));
			for (auto buffer : {myGiCache, myGiCacheRecords, myGiCacheTables, myGiCacheFree})
				builder.Use(buffer, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite));
			builder.Use(myGiStaticVoxels, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead));
			builder.Use(myGiStatisticsId, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite));
			for (auto volume : myGiVolumes)
				builder.Use(volume, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite));
		},
		[this, giKernel, computeBarrier, clips](FrameGraph::PassContext& context)
		{
			auto cmd = context.cmd;
			// each step skippable by name, to measure it (framegraph::Skipped)
			using framegraph::Skipped;
			if (!Skipped("GI Light"))
				clips([&](uint32_t clip, const gi::Layout& layout) { giKernel(cmd, kGiLightEntryPoint, layout.voxelCount, clip); });
			// the flags: the blocks near a surface (by the blocks the lighting stamped), and that spread along each axis
			// (each step reads the one before's)
			for (uint32_t stepIt = 0; stepIt < 4 && !Skipped("GI Flags"); stepIt++)
			{
				computeBarrier(cmd);
				clips([&](uint32_t clip, const gi::Layout& layout) { giKernel(cmd, kGiFlagsEntryPoint, layout.flagCount, clip, stepIt); });
			}
			computeBarrier(cmd);
			if (!Skipped("GI Trace"))
				clips([&](uint32_t clip, const gi::Layout& layout)
					  { giKernel(cmd, kGiTraceEntryPoint, static_cast<uint64_t>(layout.probeCount) * myGiRayCountThisFrame, clip); });
			computeBarrier(cmd);
			constexpr uint64_t kTexelCount = SHADER_TYPES_GI_DEPTH_SIDE * SHADER_TYPES_GI_DEPTH_SIDE;
			if (!Skipped("GI Probe Update"))
				clips([&](uint32_t clip, const gi::Layout& layout)
					  { giKernel(cmd, kGiProbeUpdateEntryPoint, static_cast<uint64_t>(layout.probeCount) * kTexelCount, clip); });
			computeBarrier(cmd);
			// the cache's storage: the free list made or grown, then the pages of the blocks no longer near a surface
			// freed, before the resolve takes pages for the blocks new to it (see GiCachePopPage)
			if (myGiSurfaceCacheOn && !Skipped("GI Cache Release"))
			{
				giKernel(cmd, kGiCachePrepareEntryPoint, myGiCachePrepareThreadCount, 0);
				computeBarrier(cmd);
				clips([&](uint32_t clip, const gi::Layout& layout) { giKernel(cmd, kGiCacheReleaseEntryPoint, layout.flagCount, clip); });
				computeBarrier(cmd);
			}
			if (!Skipped("GI Resolve"))
				clips([&](uint32_t clip, const gi::Layout& layout)
					  { giKernel(cmd, kGiResolveEntryPoint, static_cast<uint64_t>(layout.flagCount) * SHADER_TYPES_GI_GROUP_SIZE, clip); });
			// the surface cache's light filtered, a pass at a time (its noise first, see ComputeGiCacheFilter), and blended
			// into what the resolve wrote, where it is on
			if (myGiSurfaceCacheOn && !Skipped("GI Cache Filter"))
			{
				for (uint32_t passIt = 0; passIt <= myGiCacheFilterPassCount; passIt++)
				{
					computeBarrier(cmd);
					clips([&](uint32_t clip, const gi::Layout& layout)
						  { giKernel(cmd, kGiCacheFilterEntryPoint, static_cast<uint64_t>(layout.flagCount) * SHADER_TYPES_GI_GROUP_SIZE, clip, passIt); });
				}
			}
		});

	prepass(false);

	// occlusion culling: the depth pyramid of what the pre-pass drew first (the draws seen last frame), level by level
	// (each dispatch reads the one before: barriers between them, within the pass)
	myDepthPyramidPass = myGraph.AddPass(
		"Depth Pyramid",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myDepth, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myDepthPyramid, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderWrite));
		},
		[pyramid](FrameGraph::PassContext& context) { pyramid(context, 0); });
	// then the rest of the main list's draws, those it doesn't hide (and the transmissive phase's)
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
				static_cast<uint32_t>(myCullJobs.size()) - myCullMainLateJobBase,
				myCullMainLateJobBase);
		});
	myPrepassLatePass = prepass(true);

	// the traced shadows (see tracedshadows.slang): a ray per pixel and traced light through the BVHs, from the pre-pass's
	// depth, then filtered over each penumbra's width (rows, then columns), for the main pass's opaque fragments
	auto screenKernel = [this, kernel](CommandBufferHandle cmd, uint8_t entryPoint)
	{
		auto extent = myGraph.GetExtent();
		kernel(cmd, ComputePipelineVariant{.entryPoint = entryPoint}, extent.width, extent.height);
	};
	// the pixels the atlas is sure of decided, the others listed (see ComputeShadowClassify), then traced, each by a thread
	myShadowTraceClearPass = myGraph.AddPass(
		"Shadow Trace Clear",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myShadowTraceList, ResourceUse::TransferDestination());
			builder.Use(myShadowTraceTiles, ResourceUse::TransferDestination());
		},
		[this](FrameGraph::PassContext& context)
		{
			CommandEncoder encoder(context.cmd);
			encoder.FillBuffer(context.graph.GetBuffer(myShadowTraceList), 0, sizeof(uint32_t), 0);
			encoder.FillBuffer(context.graph.GetBuffer(myShadowTraceTiles), 0, 0, 0);
		});
	myShadowClassifyPass = myGraph.AddPass(
		"Shadow Classify",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myDepth, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myPrepassNormal, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myShadowAtlas, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myShadowTraceList, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite));
			builder.Use(myShadowTraceTiles, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderWrite));
			builder.Use(myShadowRaw, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderWrite));
			builder.Use(myShadowBlocker, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderWrite));
		},
		[screenKernel](FrameGraph::PassContext& context) { screenKernel(context.cmd, kShadowClassifyEntryPoint); });
	myShadowTracePass = myGraph.AddPass(
		"Shadow Trace",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myDepth, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myPrepassNormal, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myShadowAtlas, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myShadowTraceList, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead));
			builder.Use(myShadowRaw, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderWrite));
			builder.Use(myShadowBlocker, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderWrite));
		},
		[kernel](FrameGraph::PassContext& context)
		{ kernel(context.cmd, ComputePipelineVariant{.entryPoint = kShadowTraceEntryPoint}, SHADER_TYPES_SHADOW_TRACE_THREAD_COUNT, 1); });
	myShadowDenoiseXPass = myGraph.AddPass(
		"Shadow Denoise X",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myDepth, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myPrepassNormal, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myShadowRaw, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myShadowBlocker, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myShadowTraceTiles, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead));
			builder.Use(myShadowRows, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderWrite));
		},
		[screenKernel](FrameGraph::PassContext& context) { screenKernel(context.cmd, kShadowDenoiseXEntryPoint); });
	myShadowDenoiseYPass = myGraph.AddPass(
		"Shadow Denoise Y",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myDepth, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myPrepassNormal, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myShadowRows, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myShadowBlocker, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myShadowTraceTiles, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead));
			builder.Use(myShadowMask, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderWrite));
		},
		[screenKernel](FrameGraph::PassContext& context) { screenKernel(context.cmd, kShadowDenoiseYEntryPoint); });

	// the indirect lighting's near field (see ginear.slang): a ray from every other pixel each way of the pre-pass's depth
	// through the BVHs, a few voxels long (the radiance of what it meets, or the volume's light along it from its end),
	// then filtered over the traced pixels around on the same surface (rows, then columns), for the main pass's opaque
	// fragments
	myGiNearPass = myGraph.AddPass(
		"GI Near",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myDepth, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myPrepassNormal, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myGiVoxels, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead));
			builder.Use(myGiRadiance, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead));
			for (auto volume : myGiVolumes)
				builder.Use(volume, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead));
			builder.Use(myGiNearRaw, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderWrite));
		},
		[this, kernel](FrameGraph::PassContext& context)
		{
			// every myGiNearStride-th pixel each way (see ComputeGiNear)
			auto extent = myGraph.GetExtent();
			kernel(
				context.cmd,
				ComputePipelineVariant{.entryPoint = kGiNearEntryPoint},
				(extent.width + myGiNearStride - 1) / myGiNearStride,
				(extent.height + myGiNearStride - 1) / myGiNearStride);
		});
	// the filter and the frames at the traced pixels, a thread each (see ComputeGiNearX)
	auto nearKernel = [this, kernel](CommandBufferHandle cmd, uint8_t entryPoint)
	{
		auto extent = myGraph.GetExtent();
		kernel(
			cmd,
			ComputePipelineVariant{.entryPoint = entryPoint},
			(extent.width + myGiNearStride - 1) / myGiNearStride,
			(extent.height + myGiNearStride - 1) / myGiNearStride);
	};
	myGiNearXPass = myGraph.AddPass(
		"GI Near X",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myDepth, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myPrepassNormal, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myGiNearRaw, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myGiNearRows, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderWrite));
		},
		[nearKernel](FrameGraph::PassContext& context) { nearKernel(context.cmd, kGiNearXEntryPoint); });
	myGiNearYPass = myGraph.AddPass(
		"GI Near Y",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myDepth, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myPrepassNormal, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myGiNearRows, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myGiNear, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderWrite));
		},
		[nearKernel](FrameGraph::PassContext& context) { nearKernel(context.cmd, kGiNearYEntryPoint); });
	// and accumulated over frames: the last frame's history reprojected and blended with this frame's (the images the
	// frame's parity says, read and written in the general layout, see SHADER_TYPES_GI_NEAR_HISTORY_TEXTURE)
	myGiNearTemporalPass = myGraph.AddPass(
		"GI Near Temporal",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myDepth, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myPrepassNormal, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myGiNear, ResourceUse::Sampled(PipelineStage::kComputeShader));
			for (auto image : {myGiNearHistory[0], myGiNearHistory[1], myGiNearHistoryDistance[0], myGiNearHistoryDistance[1]})
				builder.Use(image, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite));
		},
		[nearKernel](FrameGraph::PassContext& context) { nearKernel(context.cmd, kGiNearTemporalEntryPoint); });

	// emissive surfaces as lights (see emitters.slang): every stride-th pixel each way of the pre-pass's depth resamples
	// emitter points into its reservoir, reusing the last frame's (and its neighbors'), and traces the chosen one's shadow
	// ray through the BVHs; the light filtered over the traced pixels around on the same surface (rows, then columns) and
	// over frames (the images of the frame's parity), a texel per traced pixel, which the main pass's opaque fragments
	// upsample
	auto tracedKernel = [this, kernel](CommandBufferHandle cmd, uint8_t entryPoint)
	{
		auto extent = myGraph.GetExtent();
		uint32_t stride = myEmitterStride;
		kernel(cmd, ComputePipelineVariant{.entryPoint = entryPoint}, (extent.width + stride - 1) / stride, (extent.height + stride - 1) / stride);
	};
	myEmitterSamplePass = myGraph.AddPass(
		"Emitter Sample",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myDepth, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myPrepassNormal, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myEmitterReservoirs, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite));
			for (auto image : myEmitterSurfaces)
				builder.Use(image, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead));
		},
		[tracedKernel](FrameGraph::PassContext& context) { tracedKernel(context.cmd, kEmitterSampleEntryPoint); });
	myEmitterShadePass = myGraph.AddPass(
		"Emitter Shade",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myDepth, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myPrepassNormal, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myEmitterReservoirs, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite));
			builder.Use(myEmitterRaw, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderWrite));
		},
		[tracedKernel](FrameGraph::PassContext& context) { tracedKernel(context.cmd, kEmitterShadeEntryPoint); });
	myEmitterXPass = myGraph.AddPass(
		"Emitter X",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myDepth, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myPrepassNormal, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myEmitterRaw, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myEmitterRows, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderWrite));
		},
		[tracedKernel](FrameGraph::PassContext& context) { tracedKernel(context.cmd, kEmitterXEntryPoint); });
	myEmitterYPass = myGraph.AddPass(
		"Emitter Y",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myDepth, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myPrepassNormal, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myEmitterRows, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myEmitterFiltered, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderWrite));
		},
		[tracedKernel](FrameGraph::PassContext& context) { tracedKernel(context.cmd, kEmitterYEntryPoint); });
	myEmitterTemporalPass = myGraph.AddPass(
		"Emitter Temporal",
		[this](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myDepth, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myPrepassNormal, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myEmitterFiltered, ResourceUse::Sampled(PipelineStage::kComputeShader));
			for (auto image : {myEmitterHistory[0], myEmitterHistory[1], myEmitterSurfaces[0], myEmitterSurfaces[1]})
				builder.Use(image, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite));
		},
		[tracedKernel](FrameGraph::PassContext& context) { tracedKernel(context.cmd, kEmitterTemporalEntryPoint); });

	mainPass(MainPassPhase::kOpaque);
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
			constexpr auto kHistogramSize = SHADER_TYPES_HISTOGRAM_BINS * sizeof(uint32_t);
			CommandEncoder(context.cmd).FillBuffer(
				context.graph.GetBuffer(myExposureHistogram), myFrame.inputs->frameIndex * kHistogramSize, kHistogramSize, 0);
		});

	// the backdrop, the transparency's resolve, exposure and tonemapping, into the swapchain image
	myGraph.AddPass(
		"ComputeMain",
		[this, &useOitLists](FrameGraph::PassBuilder& builder)
		{
			builder.Use(myColor, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myDepth, ResourceUse::Sampled(PipelineStage::kComputeShader)); // the distances' histogram
			builder.Use(myGiStatisticsId, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderWrite)); // the dump's pixel
			// the blocks the pixels' lookups read, for the surface cache (see GiMarkSeen)
			builder.Use(myPrepassNormal, ResourceUse::Sampled(PipelineStage::kComputeShader));
			builder.Use(myGiFlags, ResourceUse::Storage(PipelineStage::kComputeShader, Access::kShaderRead | Access::kShaderWrite));
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
					.imageView = myRenderOutput.GetTarget(inputs.outputIndex).GetAttachments()[0],
					.layout = myRenderOutput.GetTarget(inputs.outputIndex).GetLayout(0)},
				DESCRIPTOR_SET_CATEGORY_GLOBAL_RW_TEXTURES,
				inputs.outputIndex);

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
			auto extent = myRenderOutput.GetTarget(inputs.outputIndex).GetExtent();
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
			builder.Use(myGiStatisticsId, ResourceUse::HostRead());
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
	// the gpu culls the draws (the Cull passes) where the draws are indirect, unless SPEEDO_GPU_CULLING=0 (the cpu does,
	// by the frustum). it compacts the draws it keeps and counts them (DrawIndexedIndirectCount) where the count is native;
	// where it is emulated (KosmicKrisp: a compute kernel and an encoder split per count) every draw keeps its place, of
	// no instances if culled, drawn by plain indirect draws, which cost nothing measurable there (see CLAUDE.md)
	auto limits = device.GetLimits();
	const char* gpuCulling = std::getenv("SPEEDO_GPU_CULLING");
	myGpuCulling = myDrawStreams.front().indirect && (gpuCulling == nullptr || std::string_view(gpuCulling) != "0");
	myCountEmulated = !limits.drawIndirectCount || limits.drawIndirectCountEmulated;
	// the indirect lighting, when FrameInputs::indirectLighting says (View > Indirect Lighting, SPEEDO_GI=1 to start with
	// it), at FrameInputs::giResolution voxels along the scene's longest axis (View > Indirect Lighting Resolution,
	// SPEEDO_GI_RESOLUTION), and the surface cache's rays a voxel a frame and most samples (see GiData::cacheRayCount)
	if (const char* rays = std::getenv("SPEEDO_GI_CACHE_RAYS"); rays != nullptr && *rays != '\0')
		myGiCacheRayCount = std::clamp<uint32_t>(static_cast<uint32_t>(std::strtoul(rays, nullptr, 10)), 1U, 64U);
	if (const char* samples = std::getenv("SPEEDO_GI_CACHE_SAMPLES"); samples != nullptr && *samples != '\0')
		myGiCacheSampleCount = std::clamp<uint32_t>(static_cast<uint32_t>(std::strtoul(samples, nullptr, 10)), 1U, 0xFFFFU);
	if (const char* passes = std::getenv("SPEEDO_GI_CACHE_FILTER_PASSES"); passes != nullptr && *passes != '\0')
		myGiCacheFilterPassCount = std::clamp<uint32_t>(static_cast<uint32_t>(std::strtoul(passes, nullptr, 10)), 1U, 4U);
	if (const char* sigma = std::getenv("SPEEDO_GI_CACHE_SIGMA"); sigma != nullptr && *sigma != '\0')
		myGiCacheSigma = std::max(std::strtof(sigma, nullptr), 0.0F);
	if (const char* everywhere = std::getenv("SPEEDO_GI_CACHE_EVERYWHERE"); everywhere != nullptr)
		myGiCacheEverywhere = std::string_view(everywhere) == "1";
	if (const char* keep = std::getenv("SPEEDO_GI_CACHE_KEEP_FRAMES"); keep != nullptr && *keep != '\0')
		myGiCacheKeepFrameCount = static_cast<uint32_t>(std::strtoul(keep, nullptr, 10));
	if (const char* show = std::getenv("SPEEDO_GI_CACHE_SHOW"); show != nullptr)
		myGiCacheShow = std::string_view(show) == "1";
	// the clipmap's clips (SPEEDO_GI_CLIP_COUNT, 3 by default: the scene's, and two of half and a quarter of its voxels
	// around the camera)
	if (const char* clipCount = std::getenv("SPEEDO_GI_CLIP_COUNT"); clipCount != nullptr && *clipCount != '\0')
		myGiClipCount = std::clamp<uint32_t>(static_cast<uint32_t>(std::strtoul(clipCount, nullptr, 10)), 1, SHADER_TYPES_GI_CLIP_COUNT);
	// how far ahead of the eye the finer clips are centered, as a share of their half size (SPEEDO_GI_LOOK_AHEAD, 0.5 by
	// default; 0 centers them on the eye), toward what the camera looks at (see gi::MakeClipmap)
	if (const char* lookAhead = std::getenv("SPEEDO_GI_LOOK_AHEAD"); lookAhead != nullptr && *lookAhead != '\0')
		myGiLookAhead = std::clamp(static_cast<float>(std::strtod(lookAhead, nullptr)), 0.0F, 1.0F);
	// how near an emitter a probe's ray takes its emission as from that far away, in probe spacings
	// (SPEEDO_GI_EMISSION_REACH, 0.5 by default, 0 for none: see GiMarch)
	if (const char* reach = std::getenv("SPEEDO_GI_EMISSION_REACH"); reach != nullptr && *reach != '\0')
		myGiEmissionReach = std::clamp(static_cast<float>(std::strtod(reach, nullptr)), 0.0F, 8.0F);
	// the near field's rays (see GiData::nearReach): how far they go, in voxels of the clips that light the point
	// (SPEEDO_GI_NEAR_REACH, 8 by default; how near what they meet is a contact is FrameInputs::giNearContact), the power of
	// the share of them that met no contact which the volume's lookup is weighed by against what they gathered
	// (SPEEDO_GI_NEAR_POWER, 2), and what the shading shows of them (SPEEDO_GI_NEAR_SHOW: 1 what they gathered alone, 2
	// the share that met no contact, 3 and 4 the gathered light of the rays that met something, and of those that didn't)
	if (const char* nearReach = std::getenv("SPEEDO_GI_NEAR_REACH"); nearReach != nullptr && *nearReach != '\0')
		myGiNearReach = std::clamp(static_cast<float>(std::strtod(nearReach, nullptr)), 0.0F, 64.0F);
	if (const char* nearStride = std::getenv("SPEEDO_GI_NEAR_STRIDE"); nearStride != nullptr && *nearStride != '\0')
		myGiNearStride = std::string_view(nearStride) == "4" ? 4U : 2U;
	if (const char* nearPower = std::getenv("SPEEDO_GI_NEAR_POWER"); nearPower != nullptr && *nearPower != '\0')
		myGiNearPower = std::clamp(static_cast<float>(std::strtod(nearPower, nullptr)), 0.0F, 64.0F);
	if (const char* nearShow = std::getenv("SPEEDO_GI_NEAR_SHOW"); nearShow != nullptr && *nearShow != '\0')
		myGiNearShow = static_cast<uint32_t>(std::strtoul(nearShow, nullptr, 10));
	// the near field over frames: its history the last this many frames, about (the frame's weight in it the reciprocal)
	if (const char* frames = std::getenv("SPEEDO_GI_NEAR_HISTORY_FRAME_COUNT"); frames != nullptr && *frames != '\0')
		myGiNearBlend = 1.0F / std::clamp(static_cast<float>(std::strtod(frames, nullptr)), 1.0F, 1024.0F);
	// every clip voxelized again every frame (SPEEDO_GI_REVOXELIZE=1): what the clips' scrolling must come to, for
	// comparing with it (slow)
	const char* revoxelize = std::getenv("SPEEDO_GI_REVOXELIZE");
	myGiRevoxelize = revoxelize != nullptr && std::string_view(revoxelize) == "1";
	// where the clips are centered, a point a frame and the last kept, in place of the camera (SPEEDO_GI_EYE_PATH=
	// <x>,<y>,<z>[,<look x>,<look y>,<look z>][;...], or @<file> of them, which an environment variable on Windows can't
	// hold more than 32767 characters of): the clips moved under a camera that doesn't, so that what changes is what their
	// moves change; with a look (see gi::MakeClipmap), the frames of a run replayed (SPEEDO_GI_EYE_LOG)
	if (const char* path = std::getenv("SPEEDO_GI_EYE_PATH"); path != nullptr && *path != '\0')
	{
		std::string text = path;
		if (text.front() == '@')
		{
			auto file = core::file::Map(text.substr(1));
			ENSUREF(file.has_value(), "SPEEDO_GI_EYE_PATH: couldn't read {}", text.substr(1));
			text.assign(reinterpret_cast<const char*>(file->data()), file->size());
		}
		for (const char* it = text.c_str(); *it != '\0';)
		{
			std::array<float, 6> values{};
			int valueIt = 0;
			for (; valueIt < 6 && *it != '\0' && *it != ';' && *it != '\n'; valueIt++)
			{
				char* end = nullptr;
				values[valueIt] = std::strtof(it, &end);
				if (end == it)
					break;
				it = *end == ',' ? end + 1 : end;
			}
			if (valueIt != 3 && valueIt != 6)
			{
				std::println(stderr, "SPEEDO_GI_EYE_PATH: expected x,y,z[,lx,ly,lz][;...], got {}", path);
				myGiEyePath.clear();
				break;
			}
			myGiEyePath.push_back(GiEyePoint{
				.eye = glm::vec3(values[0], values[1], values[2]),
				.look = valueIt == 6 ? std::optional<glm::vec3>(glm::vec3(values[3], values[4], values[5])) : std::nullopt});
			while (*it == ';' || *it == '\n' || *it == '\r')
				it++;
		}
	}
	// each frame's center and look printed (SPEEDO_GI_EYE_LOG=1), as SPEEDO_GI_EYE_PATH takes them
	const char* eyeLog = std::getenv("SPEEDO_GI_EYE_LOG");
	myGiEyeLog = eyeLog != nullptr && std::string_view(eyeLog) == "1";
	// the probes' rays a frame (SPEEDO_GI_RAY_COUNT, 128 by default), over how many frames their sets of directions
	// repeat (SPEEDO_GI_RAY_PHASE_COUNT, 32), their spacing in voxels (SPEEDO_GI_PROBE_SPACING, 8, a multiple of 8), how much of what a
	// probe holds it keeps a frame (SPEEDO_GI_HYSTERESIS, 0.97: the rest is the frame's rays'), and whether the voxels
	// take the probes' light of the last frame (SPEEDO_GI_BOUNCE=0 for the lights' light alone)
	if (const char* rays = std::getenv("SPEEDO_GI_RAY_COUNT"); rays != nullptr && *rays != '\0')
		myGiRayCount = std::clamp<uint32_t>(static_cast<uint32_t>(std::strtoul(rays, nullptr, 10)), 8U, 1024U);
	// and at most this many rays a frame over all the probes traced (SPEEDO_GI_RAY_BUDGET, 500000): a scene filling its
	// clips with surfaces traces every probe near them (DamagedHelmet 10400 of its 98000, which at 128 rays cost 10 ms)
	if (const char* budget = std::getenv("SPEEDO_GI_RAY_BUDGET"); budget != nullptr && *budget != '\0')
		myGiRayBudget = std::max<uint32_t>(static_cast<uint32_t>(std::strtoul(budget, nullptr, 10)), 1U);
	if (const char* phases = std::getenv("SPEEDO_GI_RAY_PHASE_COUNT"); phases != nullptr && *phases != '\0')
		myGiRayPhaseCount = std::clamp<uint32_t>(static_cast<uint32_t>(std::strtoul(phases, nullptr, 10)), 1U, 256U);
	if (const char* spacing = std::getenv("SPEEDO_GI_PROBE_SPACING"); spacing != nullptr && *spacing != '\0')
		myGiProbeSpacing = std::clamp<uint32_t>(static_cast<uint32_t>(std::strtoul(spacing, nullptr, 10)), 8U, 32U);
	// a multiple of 8: the resolve's blocks of 4 voxels then never span two probe cells (see ComputeGiResolve)
	ENSUREF(myGiProbeSpacing % 8 == 0, "SPEEDO_GI_PROBE_SPACING must be a multiple of 8, not {}", myGiProbeSpacing);
	if (const char* hysteresis = std::getenv("SPEEDO_GI_HYSTERESIS"); hysteresis != nullptr && *hysteresis != '\0')
		myGiHysteresis = std::clamp(std::strtof(hysteresis, nullptr), 0.0F, 0.99F);
	if (const char* timeBased = std::getenv("SPEEDO_GI_TIME_BASED"); timeBased != nullptr && *timeBased != '\0')
		myGiTimeBased = std::string_view(timeBased) != "0";
	const char* bounce = std::getenv("SPEEDO_GI_BOUNCE");
	if (const char* rays = std::getenv("SPEEDO_GI_REFERENCE_RAYS"); rays != nullptr && *rays != '\0')
		myGiReferenceRayCount = std::clamp<uint32_t>(static_cast<uint32_t>(std::strtoul(rays, nullptr, 10)), 16U, 4096U);
	myGiBounce = bounce == nullptr || std::string_view(bounce) != "0";
	// and whether a probe blends faster through a change of the light (SPEEDO_GI_ADAPT=1; off: on a still scene the
	// same ray set returns every rayPhaseCount frames, and a ray that hits a sunlit patch tripped the test each time,
	// a jump of a quarter every 64 frames in Sponza's arcade)
	const char* adapt = std::getenv("SPEEDO_GI_ADAPT");
	myGiAdapt = adapt != nullptr && std::string_view(adapt) == "1";
	// a probe dumped at exit for debugging (SPEEDO_GI_DUMP=<x,y,z>, or pixel:<x>,<y> for the point under a pixel: per
	// clip the one nearest the point, see GI_DUMP_FLOAT_COUNT)
	if (const char* dump = std::getenv("SPEEDO_GI_DUMP"); dump != nullptr && *dump != '\0')
	{
		std::string_view text(dump);
		bool pixel = text.starts_with("pixel:");
		const char* it = dump + (pixel ? 6 : 0);
		int axisIt = 0;
		for (; axisIt < (pixel ? 2 : 3); axisIt++)
		{
			char* end = nullptr;
			myGiDumpPoint[axisIt] = std::strtof(it, &end);
			if (end == it)
				break;
			it = *end == ',' ? end + 1 : end;
		}
		myGiDump = axisIt == (pixel ? 2 : 3);
		if (myGiDump && pixel)
		{
			myGiDumpPixel = glm::ivec2(glm::round(glm::vec2(myGiDumpPoint)));
			myGiDumpPoint = glm::vec3(0.0F);
		}
	}
	// occlusion culling of the main pass (SPEEDO_OCCLUSION_CULLING=0 turns it off), and of the shadow views
	// (SPEEDO_SHADOW_OCCLUSION_CULLING=1 turns it on: off by default, since a shadow view sees few of the chunks hidden
	// behind others, 6 to 11 of Bistro's 560 to 800, and its pyramid and the atlas's second render pass cost 0.5 to 1 ms)
	const char* occlusion = std::getenv("SPEEDO_OCCLUSION_CULLING");
	myOcclusionCulling = myGpuCulling && (occlusion == nullptr || std::string_view(occlusion) != "0");
	const char* shadowOcclusion = std::getenv("SPEEDO_SHADOW_OCCLUSION_CULLING");
	myShadowOcclusionCulling = myGpuCulling && shadowOcclusion != nullptr && std::string_view(shadowOcclusion) == "1";
	if (const char* mode = std::getenv("SPEEDO_OCCLUSION_MODE"); mode != nullptr)
	{
		std::string_view name(mode);
		myOcclusionMode = name == "reprojected" ? OcclusionMode::kReprojected
						  : name == "hybrid"	? OcclusionMode::kHybrid
												: OcclusionMode::kTwoPhase;
	}
	// the emitters' sampling (see EmitterData): the candidates a traced pixel resamples each frame, a voxel's samples and
	// the candidates of each, the reservoirs' reuse, and the frames the light is accumulated over, about
	auto count = [](const char* name, uint32_t& value, uint32_t least, uint32_t most)
	{
		if (const char* text = std::getenv(name); text != nullptr && *text != '\0')
			value = std::clamp<uint32_t>(static_cast<uint32_t>(std::strtoul(text, nullptr, 10)), least, most);
	};
	count("SPEEDO_EMITTER_CANDIDATE_COUNT", myEmitterCandidateCount, 1, 256);
	count("SPEEDO_EMITTER_VOXEL_SAMPLE_COUNT", myEmitterVoxelSampleCount, 1, 256);
	count("SPEEDO_EMITTER_VOXEL_CANDIDATE_COUNT", myEmitterVoxelCandidateCount, 1, 256);
	count("SPEEDO_EMITTER_SPATIAL_COUNT", myEmitterSpatialCount, 0, 8); // see kEmitterSpatialCountMost
	// the filter's reach, in pixels (at least a traced pixel each way)
	count("SPEEDO_EMITTER_FILTER_REACH", myEmitterFilterReach, 1, 64);
	if (const char* cap = std::getenv("SPEEDO_EMITTER_HISTORY_CAP"); cap != nullptr && *cap != '\0')
		myEmitterHistoryCap = std::clamp(std::strtof(cap, nullptr), 1.0F, 1000.0F);
	const char* reuse = std::getenv("SPEEDO_EMITTER_REUSE");
	myEmitterReuse = reuse != nullptr && std::string_view(reuse) == "1";
	if (const char* frames = std::getenv("SPEEDO_EMITTER_HISTORY_FRAME_COUNT"); frames != nullptr && *frames != '\0')
		myEmitterBlend = 1.0F / std::clamp(static_cast<float>(std::strtod(frames, nullptr)), 1.0F, 1024.0F);
	if (const char* roulette = std::getenv("SPEEDO_EMITTER_ROULETTE"); roulette != nullptr && *roulette != '\0')
		myEmitterRoulette = std::max(std::strtof(roulette, nullptr), 0.0F);
	count("SPEEDO_EMITTER_VISIBILITY_FRAME_COUNT", myEmitterVisibilityFrameCount, 1, 1024);
	const char* collect = std::getenv("SPEEDO_GPU_TIMINGS");
	myCollectTimings = collect != nullptr && std::string_view(collect) == "1";
}

Renderer::~Renderer()
{
	// what the occlusion culling decided, on average per frame (SPEEDO_GPU_TIMINGS=1)
	if (myCollectTimings && myCullStatisticFrameCount > 0)
	{
		for (uint32_t groupIt = 0; groupIt < CULL_STATISTIC_GROUP_COUNT; groupIt++)
		{
			auto mean = [this, groupIt](uint32_t statistic)
			{
				return static_cast<double>(myCullStatisticSums[(groupIt * CULL_STATISTIC_COUNT) + statistic]) /
					   static_cast<double>(myCullStatisticFrameCount);
			};
			std::println(
				"culling of the {} (occlusion {}), draws per frame: {:.1f} early, {:.1f} late ({:.1f} blended), {:.1f} hidden, {:.1f} outside "
				"the frustum, {:.0f} triangles drawn ({} frames)",
				groupIt == CULL_STATISTIC_MAIN ? "main views" : "shadow views",
				groupIt == CULL_STATISTIC_MAIN ? GetOcclusionModeName() : (myShadowOcclusionCulling ? "two-phase" : "off"),
				mean(CULL_STATISTIC_EARLY),
				mean(CULL_STATISTIC_LATE) + mean(CULL_STATISTIC_LATE_BLENDED),
				mean(CULL_STATISTIC_LATE_BLENDED),
				mean(CULL_STATISTIC_HIDDEN),
				mean(CULL_STATISTIC_FRUSTUM),
				mean(CULL_STATISTIC_TRIANGLES),
				myCullStatisticFrameCount);
		}
	}
	// and the indirect lighting's clips as they were last (origin, voxels, voxel size, the box around its center), its probes
	// traced and rays per probe, on average per frame
	if (myCollectTimings && myGiStatisticFrameCount > 0)
		for (const auto& clip : myGiClipmap.clips)
		{
			const auto& data = clip.data;
			std::println(
				"indirect lighting, clip {}: origin ({:.2f}, {:.2f}, {:.2f}), {}x{}x{} voxels of {:.3f}, probes {}x{}x{} every {}, its box around ({:.2f}, {:.2f}, {:.2f}) +- ({:.2f}, {:.2f}, {:.2f}) fading over {:.2f}",
				data.clip,
				static_cast<double>(data.volumeOrigin[0]),
				static_cast<double>(data.volumeOrigin[1]),
				static_cast<double>(data.volumeOrigin[2]),
				data.volumeSize[0],
				data.volumeSize[1],
				data.volumeSize[2],
				static_cast<double>(data.volumeOrigin[3]),
				data.probeSize[0],
				data.probeSize[1],
				data.probeSize[2],
				data.probeSize[3],
				static_cast<double>(data.fadeCenter[0]),
				static_cast<double>(data.fadeCenter[1]),
				static_cast<double>(data.fadeCenter[2]),
				static_cast<double>(data.fadeExtent[0]),
				static_cast<double>(data.fadeExtent[1]),
				static_cast<double>(data.fadeExtent[2]),
				static_cast<double>(data.fadeExtent[3]));
		}
	// and the dumped probes (SPEEDO_GI_DUMP), as they were last
	if (myGiDump)
		for (size_t clipIt = 0; clipIt < myGiClipmap.clips.size(); clipIt++)
		{
			const float* values = myGiDumpValues.data() + clipIt * GI_DUMP_FLOAT_COUNT;
			std::println(
				"indirect lighting, clip {}'s probe nearest ({:.2f}, {:.2f}, {:.2f}): at ({:.3f}, {:.3f}, {:.3f}), offset ({:.3f}, {:.3f}, {:.3f}), age {:.0f}, inside {:.0f}, {:.0f} of {:.0f} rays met a back; depth map (rows from the map's bottom, spacings, negative provisional):",
				clipIt, static_cast<double>(myGiDumpPoint.x), static_cast<double>(myGiDumpPoint.y), static_cast<double>(myGiDumpPoint.z),
				static_cast<double>(values[0]), static_cast<double>(values[1]), static_cast<double>(values[2]),
				static_cast<double>(values[3]), static_cast<double>(values[4]), static_cast<double>(values[5]),
				static_cast<double>(values[6]), static_cast<double>(values[7]), static_cast<double>(values[8]), static_cast<double>(values[9]));
			std::println(
				"  lattice point at voxel ({:.4f}, {:.4f}, {:.4f}); the column of voxels through it, 4 below to 3 above (1 occupied): {:08b}",
				static_cast<double>(values[10]), static_cast<double>(values[11]), static_cast<double>(values[12]), static_cast<uint32_t>(values[13]));
			std::println(
				"  straight down from where the rays were traced: the box exit at {:.3f} spacings, the walk meets a voxel at {:.3f}",
				static_cast<double>(values[14]), static_cast<double>(values[15]));
			std::string rays;
			for (uint32_t rayIt = 0; rayIt < 16; rayIt++)
				rays += std::format(" ({:+.2f}: {:.2f})", static_cast<double>(values[16 + 256 + rayIt * 2]), static_cast<double>(values[16 + 256 + rayIt * 2 + 1]));
			std::println("  the first rays (direction's y: depth):{}", rays);
			std::println(
				"  its light (luminance, bands 0 and 1) along +x {:.5f} -x {:.5f} +y {:.5f} -y {:.5f} +z {:.5f} -z {:.5f}; this frame's rays' mean {:.5f}",
				static_cast<double>(values[16 + 256 + 36]), static_cast<double>(values[16 + 256 + 37]), static_cast<double>(values[16 + 256 + 38]),
				static_cast<double>(values[16 + 256 + 39]), static_cast<double>(values[16 + 256 + 40]), static_cast<double>(values[16 + 256 + 41]), static_cast<double>(values[16 + 256 + 42]));
			std::println(
				"  the volumes at its voxel (last frame's resolve): red along +x, -x, +y, -y ({:.5f}, {:.5f}, {:.5f}, {:.5f}), blue along +z, -z ({:.5f}, {:.5f}, {:.5f}) and the weight {:.5f}",
				static_cast<double>(values[16 + 256 + 43]), static_cast<double>(values[16 + 256 + 44]), static_cast<double>(values[16 + 256 + 45]), static_cast<double>(values[16 + 256 + 46]),
				static_cast<double>(values[16 + 256 + 47]), static_cast<double>(values[16 + 256 + 48]), static_cast<double>(values[16 + 256 + 49]), static_cast<double>(values[16 + 256 + 50]));
			{
				std::string line = "  from the point along -z, per voxel (occupied, red along +x, weight):";
				for (int stepIt = 0; stepIt < 16; stepIt++)
					line += std::format(" {}:{}/{:.3f}/{:.2f}", stepIt, values[16 + 256 + 51 + stepIt * 3] != 0.0F ? "x" : ".", static_cast<double>(values[16 + 256 + 52 + stepIt * 3]), static_cast<double>(values[16 + 256 + 53 + stepIt * 3]));
				std::println("{}", line);
			}
			for (int cornerIt = 0; cornerIt < 8; cornerIt++)
			{
				const float* v = &values[16 + 256 + 99 + cornerIt * 10];
				std::println("  the resolve at the point's voxel, probe {}: at ({:.1f}, {:.1f}, {:.1f}) facing {:.3f} visibility {:.3f} distance {:.3f} mean {:.3f} nearest {:.3f} weight {:.4f} red {:.3f}",
							 cornerIt, static_cast<double>(v[0]), static_cast<double>(v[1]), static_cast<double>(v[2]), static_cast<double>(v[3]), static_cast<double>(v[4]), static_cast<double>(v[5]),
							 static_cast<double>(v[6]), static_cast<double>(v[7]), static_cast<double>(v[8]), static_cast<double>(v[9]));
			}
			std::println(
				"  the walk straight down: distance {:.3f} m, ended in clip {:.0f} after {:.0f} steps, in voxel row {:.0f}",
				static_cast<double>(values[16 + 256 + 32]), static_cast<double>(values[16 + 256 + 33]), static_cast<double>(values[16 + 256 + 34]), static_cast<double>(values[16 + 256 + 35]));
			{
				// the point's voxel's surface cache entry, where it has one this frame (see GI_DUMP_CACHE_FIRST)
				const float* c = &values[GI_DUMP_CACHE_FIRST];
				auto d = [](float value) { return static_cast<double>(value); };
				if (c[0] == 0.0F && c[1] == 0.0F)
					std::println("  no surface cache entry at the point's voxel");
				else
				{
					std::println(
						"  the surface cache entry at the point's voxel: samples {:.0f}, inside {:.0f} (valid {:.0f}), surface {:.0f}, fresh {:.0f}, active {:.0f}, {:.0f} rays from ({:.3f}, {:.3f}, {:.3f}) walking clip {:.0f}; the voxel at ({:.3f}, {:.3f}, {:.3f}): occupied {:.0f}, faces {:.0f}, double sided {:.0f}, normal ({:.2f}, {:.2f}, {:.2f}), open side ({:.2f}, {:.2f}, {:.2f})",
						d(c[0]), d(c[1]), d(c[17]), d(c[18]), d(c[19]), d(c[20]), d(c[27]), d(c[2]), d(c[3]), d(c[4]), d(c[21]), d(c[5]), d(c[6]), d(c[7]),
						d(c[26]), d(c[25]), d(c[28]), d(c[22]), d(c[23]), d(c[24]), d(c[8]), d(c[9]), d(c[10]));
					std::println(
						"    its light (luminance) along +x {:.4f} -x {:.4f} +y {:.4f} -y {:.4f} +z {:.4f} -z {:.4f}", d(c[11]), d(c[12]), d(c[13]), d(c[14]), d(c[15]), d(c[16]));
					for (int rayIt = 0; rayIt < std::min(8, static_cast<int>(c[27])); rayIt++)
					{
						const float* r = &c[30 + rayIt * 6];
						std::println(
							"    ray {}: direction ({:+.2f}, {:+.2f}, {:+.2f}), distance {:.3f} (-1: met nothing), light {:.4f}, a surface's back {:.0f}",
							rayIt, d(r[0]), d(r[1]), d(r[2]), d(r[3]), d(r[4]), d(r[5]));
					}
				}
			}
			{
				uint32_t emptyCount = 0;
				uint32_t provisionalCount = 0;
				for (uint32_t texelIt = 0; texelIt < SHADER_TYPES_GI_DEPTH_SIDE * SHADER_TYPES_GI_DEPTH_SIDE; texelIt++)
				{
					float mean = values[16 + texelIt];
					emptyCount += std::abs(mean) < 1e-3F ? 1 : 0;
					provisionalCount += mean < -1e-3F ? 1 : 0;
				}
				std::println("  texels: {} empty, {} provisional", emptyCount, provisionalCount);
			}
			for (uint32_t rowIt = 0; rowIt < SHADER_TYPES_GI_DEPTH_SIDE; rowIt++)
			{
				std::string row;
				for (uint32_t columnIt = 0; columnIt < SHADER_TYPES_GI_DEPTH_SIDE; columnIt++)
					row += std::format("{:6.2f}", static_cast<double>(values[16 + (rowIt * SHADER_TYPES_GI_DEPTH_SIDE) + columnIt]));
				std::println("  {}", row);
			}
		}
	if (myCollectTimings && myGiStatisticFrameCount > 0)
		std::println(
			"indirect lighting, per frame: {:.0f} probes traced, {:.1f} rays a probe ({} frames)",
			static_cast<double>(myGiActiveProbeSum) / static_cast<double>(myGiStatisticFrameCount),
			static_cast<double>(myGiRayCountSum) / static_cast<double>(myGiStatisticFrameCount),
			myGiStatisticFrameCount);
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

std::array<uint32_t, CULL_STATISTIC_COUNT * CULL_STATISTIC_GROUP_COUNT> Renderer::GetCullStatistics() const
{
	return myCullStatistics.Read().Get();
}

FrameStatistics Renderer::GetFrameStatistics() const
{
	return myFrameStatistics.Read().Get();
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

	// with the layouts the passes leave them in (so that later writes of the same are skipped as unchanged)
	pipeline.BindLayoutAuto(pipeline.GetLayoutHandle("Main"), PipelineBindPoint::kCompute);
	pipeline.SetDescriptorData(
		"gTextures",
		ImageBinding{.sampler = {}, .imageView = myGraph.GetView(myColor), .layout = ImageLayout::kShaderReadOnly},
		DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES,
		SHADER_TYPES_RENDER_TARGET_TEXTURE);
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
			 std::pair{myUiBlurRows, SHADER_TYPES_UI_BLUR_ROWS_TEXTURE},
			 std::pair{myPrepassNormal, SHADER_TYPES_PREPASS_NORMAL_TEXTURE},
			 std::pair{myShadowRaw, SHADER_TYPES_SHADOW_RAW_TEXTURE},
			 std::pair{myShadowBlocker, SHADER_TYPES_SHADOW_BLOCKER_TEXTURE},
			 std::pair{myShadowRows, SHADER_TYPES_SHADOW_ROWS_TEXTURE},
			 std::pair{myShadowMask, SHADER_TYPES_SHADOW_MASK_TEXTURE},
			 std::pair{myGiNearRaw, SHADER_TYPES_GI_NEAR_RAW_TEXTURE},
			 std::pair{myGiNearRows, SHADER_TYPES_GI_NEAR_ROWS_TEXTURE},
			 std::pair{myGiNear, SHADER_TYPES_GI_NEAR_TEXTURE},
			 std::pair{myEmitterRaw, SHADER_TYPES_EMITTER_RAW_TEXTURE},
			 std::pair{myEmitterRows, SHADER_TYPES_EMITTER_ROWS_TEXTURE},
			 std::pair{myEmitterFiltered, SHADER_TYPES_EMITTER_FILTERED_TEXTURE}})
		pipeline.SetDescriptorData(
			"gTextures",
			ImageBinding{.sampler = {}, .imageView = myGraph.GetView(image), .layout = ImageLayout::kShaderReadOnly},
			DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES,
			slot);
	for (auto [image, slot] : std::array{
			 std::pair{myScatterNoisy, SHADER_TYPES_SCATTER_NOISY_RW_TEXTURE},
			 std::pair{myScatterLight, SHADER_TYPES_SCATTER_LIGHT_RW_TEXTURE},
			 std::pair{myUiBlurSource, SHADER_TYPES_UI_BLUR_SOURCE_RW_TEXTURE},
			 std::pair{myUiBlurRows, SHADER_TYPES_UI_BLUR_ROWS_RW_TEXTURE},
			 std::pair{myShadowRaw, SHADER_TYPES_SHADOW_RAW_RW_TEXTURE},
			 std::pair{myShadowBlocker, SHADER_TYPES_SHADOW_BLOCKER_RW_TEXTURE},
			 std::pair{myShadowRows, SHADER_TYPES_SHADOW_ROWS_RW_TEXTURE},
			 std::pair{myShadowMask, SHADER_TYPES_SHADOW_MASK_RW_TEXTURE},
			 std::pair{myGiNearRaw, SHADER_TYPES_GI_NEAR_RAW_RW_TEXTURE},
			 std::pair{myGiNearRows, SHADER_TYPES_GI_NEAR_ROWS_RW_TEXTURE},
			 std::pair{myGiNear, SHADER_TYPES_GI_NEAR_RW_TEXTURE},
			 std::pair{myEmitterRaw, SHADER_TYPES_EMITTER_RAW_RW_TEXTURE},
			 std::pair{myEmitterRows, SHADER_TYPES_EMITTER_ROWS_RW_TEXTURE},
			 std::pair{myEmitterFiltered, SHADER_TYPES_EMITTER_FILTERED_RW_TEXTURE}})
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
		pipeline.SetDescriptorData(
			"gShadowTrace",
			BufferBinding{.buffer = myShadowTraceBuffers[frameIt], .offset = 0},
			DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES,
			frameIt);
		pipeline.SetDescriptorData(
			"gEmitterData",
			BufferBinding{.buffer = myEmitterDataBuffers[frameIt], .offset = 0},
			DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES,
			frameIt);
		for (auto [name, buffers] : std::array{
				 std::pair{"gClusterViews", &myClusterViewBuffers},
				 std::pair{"gClusters", &myClusterBuffers},
				 std::pair{"gClusterLights", &myClusterLightBuffers},
				 std::pair{"gUiBlurRects", &myUiBlurRectBuffers},
				 std::pair{"gCullJobs", &myCullJobBuffers},
				 std::pair{"gCullStatistics", &myCullStatisticsBuffers},
				 std::pair{"gGiStatistics", &myGiStatisticsBuffers},
				 std::pair{"gGi", &myGiDataBuffers},
				 std::pair{"gGiJobs", &myGiJobs.buffers},
				 std::pair{"gGiLightCells", &myGiLightCells.buffers},
				 std::pair{"gGiLightIndices", &myGiLightIndices.buffers},
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
	// and the shadow atlas's pyramid, likewise
	{
		const auto& pyramid = myGraph.GetImage(myShadowDepthPyramid);
		auto levelCount = std::min<size_t>(pyramid.GetDesc().mipLevels.size(), SHADER_TYPES_SHADOW_DEPTH_PYRAMID_LEVEL_COUNT);
		myShadowDepthPyramidViews.clear();
		for (uint32_t levelIt = 0; levelIt < levelCount; levelIt++)
			myShadowDepthPyramidViews.push_back(std::make_unique<ImageView>(ImageViewCreateDesc{
				device.CreateDeviceObjectCreateDesc(std::format("Shadow Depth Pyramid Level {}", levelIt)),
				pyramid,
				Format::kR32Sfloat,
				ImageAspect::kColor,
				1,
				{},
				levelIt}));
		for (uint32_t slotIt = 0; slotIt < SHADER_TYPES_SHADOW_DEPTH_PYRAMID_LEVEL_COUNT; slotIt++)
			pipeline.SetDescriptorData(
				"gShadowDepthPyramid",
				ImageBinding{
					.sampler = {},
					.imageView = *myShadowDepthPyramidViews[std::min<size_t>(slotIt, levelCount - 1)],
					.layout = ImageLayout::kGeneral},
				DESCRIPTOR_SET_CATEGORY_GLOBAL_RW_TEXTURES,
				slotIt);
	}
	pipeline.SetDescriptorData(
		"gCullVisibility", BufferBinding{.buffer = myCullVisibilityBuffer, .offset = 0}, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES);
	for (auto [name, buffer] : std::array{
			 std::pair{"gGiVoxels", &myGiVoxelBuffer},
			 std::pair{"gGiRadiance", &myGiRadianceBuffer},
			 std::pair{"gGiRays", &myGiRayBuffer},
			 std::pair{"gGiIrradiance", &myGiIrradianceBuffer},
			 std::pair{"gGiProbeDepth", &myGiProbeDepthBuffer},
			 std::pair{"gGiFlags", &myGiFlagBuffer},
			 std::pair{"gGiVisibility", &myGiVisibilityBuffer},
			 std::pair{"gGiCache", &myGiCacheBuffer},
			 std::pair{"gGiCacheRecords", &myGiCacheRecordBuffer},
			 std::pair{"gGiCacheBlocks", &myGiCacheTableBuffer},
			 std::pair{"gGiCacheFree", &myGiCacheFreeBuffer},
			 std::pair{"gGiStaticVoxels", &myGiStaticVoxelBuffer}})
		pipeline.SetDescriptorData(name, BufferBinding{.buffer = *buffer, .offset = 0}, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES);
	// a voxel each until a model is voxelized (a depth of 2: a volume)
	if (!myGiVolumeImages[0].image)
		InternalReplaceGiVolumes(pipeline, glm::uvec3(1, 1, 2));
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
	// the near field's history at this size (holding nothing yet), bound once, both of each: the frame's parity picks
	// (and the emitters', a texel per traced pixel)
	Extent2d traced{.width = (extent.width + myEmitterStride - 1) / myEmitterStride, .height = (extent.height + myEmitterStride - 1) / myEmitterStride};
	auto screenImage = [&device](const std::string& name, Format format, Extent2d size)
	{
		ScreenImage made;
		ImageCreateDesc desc{device.CreateDeviceObjectCreateDesc(name)};
		desc.mipLevels = {{.extent = size}};
		desc.format = format;
		desc.tiling = ImageTiling::kOptimal;
		desc.usageFlags = ImageUsage::kStorage | ImageUsage::kSampled;
		desc.memoryFlags = MemoryProperty::kDeviceLocal;
		desc.imageAspectFlags = ImageAspect::kColor;
		desc.layout = ImageLayout::kUndefined;
		made.image = std::make_unique<Image>(std::move(desc));
		made.view = std::make_unique<ImageView>(
			ImageViewCreateDesc{device.CreateDeviceObjectCreateDesc(name + " View"), *made.image, format, ImageAspect::kColor});
		return made;
	};
	for (uint32_t historyIt = 0; historyIt < 2; historyIt++)
	{
		myGiNearHistoryImages[historyIt] = screenImage(std::format("GI Near History {}", historyIt), Format::kR16G16B16A16Sfloat, extent);
		// and its traced pixels' surfaces (point and packed normal, as the emitters'), which the main pass upsamples by
		myGiNearHistoryDistanceImages[historyIt] =
			screenImage(std::format("GI Near Surface {}", historyIt), Format::kR32G32B32A32Sfloat, extent);
		// and the emitters' light's and its surfaces' (see SHADER_TYPES_EMITTER_SURFACE_TEXTURE)
		myEmitterHistoryImages[historyIt] = screenImage(std::format("Emitter History {}", historyIt), Format::kR16G16B16A16Sfloat, traced);
		myEmitterSurfaceImages[historyIt] = screenImage(std::format("Emitter Surface {}", historyIt), Format::kR32G32B32A32Sfloat, traced);
		for (auto [image, slot, rwSlot] : std::array{
				 std::tuple{&myGiNearHistoryImages[historyIt], SHADER_TYPES_GI_NEAR_HISTORY_TEXTURE, SHADER_TYPES_GI_NEAR_HISTORY_RW_TEXTURE},
				 std::tuple{
					 &myGiNearHistoryDistanceImages[historyIt],
					 SHADER_TYPES_GI_NEAR_HISTORY_DISTANCE_TEXTURE,
					 SHADER_TYPES_GI_NEAR_HISTORY_DISTANCE_RW_TEXTURE},
				 std::tuple{&myEmitterHistoryImages[historyIt], SHADER_TYPES_EMITTER_HISTORY_TEXTURE, SHADER_TYPES_EMITTER_HISTORY_RW_TEXTURE},
				 std::tuple{&myEmitterSurfaceImages[historyIt], SHADER_TYPES_EMITTER_SURFACE_TEXTURE, SHADER_TYPES_EMITTER_SURFACE_RW_TEXTURE}})
		{
			pipeline.SetDescriptorData(
				"gTextures",
				ImageBinding{.sampler = {}, .imageView = *image->view, .layout = ImageLayout::kGeneral},
				DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES,
				slot + historyIt);
			pipeline.SetDescriptorData(
				"gRWTextures",
				ImageBinding{.sampler = {}, .imageView = *image->view, .layout = ImageLayout::kGeneral},
				DESCRIPTOR_SET_CATEGORY_GLOBAL_RW_TEXTURES,
				rwSlot + historyIt);
		}
	}
	myGiNearHistoryValid = false;
	myEmitterHistoryValid = false;
	// the emitters' reservoirs at this size (see ComputeEmitterShade): per traced pixel a uint4, in three parts: the frame's
	// before their spatial reuse, and after it the frame's and the last frame's
	{
		uint64_t tracedCount = static_cast<uint64_t>(traced.width) * traced.height;
		myEmitterReservoirBuffer = Buffer(BufferCreateDesc{
			device.CreateDeviceObjectCreateDesc("Emitter Reservoirs"),
			std::max<uint64_t>(3 * tracedCount, 1) * 4 * sizeof(uint32_t),
			BufferUsage::kStorage,
			MemoryProperty::kDeviceLocal});
		pipeline.SetDescriptorData(
			"gEmitterReservoirs", BufferBinding{.buffer = myEmitterReservoirBuffer, .offset = 0}, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES);
	}
	for (auto [name, buffer] : std::array{
			 std::pair{"gOitHeads", myOitHeads}, std::pair{"gOitNodes", myOitNodes}, std::pair{"gOitCounter", myOitCounter}})
		pipeline.SetDescriptorData(
			name, BufferBinding{.buffer = myGraph.GetBuffer(buffer), .offset = 0}, DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);
	pipeline.SetDescriptorData(
		"gShadowTraceList", BufferBinding{.buffer = myGraph.GetBuffer(myShadowTraceList), .offset = 0}, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES);
	pipeline.SetDescriptorData(
		"gShadowTraceTiles", BufferBinding{.buffer = myGraph.GetBuffer(myShadowTraceTiles), .offset = 0}, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES);
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
		std::array<uint32_t, CULL_STATISTIC_COUNT * CULL_STATISTIC_GROUP_COUNT> statistics{};
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
	// and what the indirect lighting's probes counted (see gGiStatistics), emptied for this frame
	{
		auto& buffer = myGiStatisticsBuffers[inputs.frameIndex];
		auto memory = buffer.Map();
		std::array<uint32_t, GI_STATISTIC_COUNT> statistics{};
		buffer.Invalidate(0, sizeof(statistics));
		std::memcpy(statistics.data(), memory.data(), sizeof(statistics));
		myGiActiveProbeCount = statistics[GI_STATISTIC_ACTIVE_PROBES];
		// what the surface cache's pools must hold (see InternalPlanGi)
		myGiCacheBlocksCounted = statistics[GI_STATISTIC_CACHE_BLOCKS];
		myGiCacheActiveBlocksCounted = statistics[GI_STATISTIC_CACHE_ACTIVE_BLOCKS];
		if (statistics[GI_STATISTIC_CAPPED_RAYS] != 0)
		{
			auto f = [&](uint32_t index) { return std::bit_cast<float>(statistics[index]); };
			std::println(stderr, "indirect lighting: {} rays hit the walk's step cap, one from ({:.4f}, {:.4f}, {:.4f}) along ({:.6f}, {:.6f}, {:.6f})",
						 statistics[GI_STATISTIC_CAPPED_RAYS], f(GI_STATISTIC_CAPPED_ORIGIN), f(GI_STATISTIC_CAPPED_ORIGIN + 1), f(GI_STATISTIC_CAPPED_ORIGIN + 2),
						 f(GI_STATISTIC_CAPPED_DIRECTION), f(GI_STATISTIC_CAPPED_DIRECTION + 1), f(GI_STATISTIC_CAPPED_DIRECTION + 2));
			std::println(stderr, "  in clip {} at t {:.4f} of {:.4f} (box exit {:.4f}), voxel ({}, {}, {}), travelled {:.4f}", f(GI_STATISTIC_CAPPED_STATE),
						 f(GI_STATISTIC_CAPPED_STATE + 1), f(GI_STATISTIC_CAPPED_STATE + 2), f(GI_STATISTIC_CAPPED_STATE + 3), f(GI_STATISTIC_CAPPED_STATE + 4),
						 f(GI_STATISTIC_CAPPED_STATE + 5), f(GI_STATISTIC_CAPPED_STATE + 6), f(GI_STATISTIC_CAPPED_STATE + 7));
		}
		if (statistics[GI_STATISTIC_CAPPED_BVH_RAYS] != 0)
			std::println(
				stderr, "indirect lighting: {} rays hit the BVH traversal's step cap (BVH_MAX_STEP_COUNT)", statistics[GI_STATISTIC_CAPPED_BVH_RAYS]);
		if (myGiDump)
		{
			constexpr size_t kDumpSize = SHADER_TYPES_GI_CLIP_COUNT * GI_DUMP_FLOAT_COUNT * sizeof(float);
			buffer.Invalidate(sizeof(statistics), kDumpSize);
			std::memcpy(myGiDumpValues.data(), memory.data() + sizeof(statistics), kDumpSize);
			// the point under the pixel, as ComputeMain found it (see GiData::dumpPixel)
			if (myGiDumpPixel && statistics[GI_STATISTIC_DUMP_POINT + 3] != 0)
				for (int axisIt = 0; axisIt < 3; axisIt++)
					myGiDumpPoint[axisIt] = std::bit_cast<float>(statistics[GI_STATISTIC_DUMP_POINT + static_cast<uint32_t>(axisIt)]);
		}
		if (myCollectTimings && myGiActiveProbeCount > 0)
		{
			myGiActiveProbeSum += myGiActiveProbeCount;
			myGiRayCountSum += myGiRayCountThisFrame;
			myGiStatisticFrameCount++;
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
	myFrameStatisticsThisFrame = FrameStatistics{
		.culling = frame.culling,
		.gpuCulling = myGpuCulling,
		.mainViewCount = frame.grid.x * frame.grid.y,
		.lightCount = static_cast<uint32_t>(frame.lights.size()),
		.clusteredLights = frame.clusteredLights};

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
	// the first phase's opaque draws shade what the pre-pass drew (the blended ones depth test as before, and the
	// transmissive phase's draws aren't in the pre-pass: the opaque scene behind them must be drawn)
	for (auto& item : myDrawLists[std::to_underlying(MainPassPhase::kOpaque)].items)
		if (item.variant.fragmentShader == 0)
			item.variant.depthTest = DepthTest::kEqual;
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
	// the traced shadows: the directional lights' (the brightest first), then the point and spot lights' the atlas has
	// views of (in view), the brightest at the camera first, up to SHADER_TYPES_SHADOW_TRACE_SLOT_COUNT, where the model
	// has BVHs to trace
	{
		ShadowTraceData trace{};
		std::ranges::fill(trace.lights, SHADER_TYPES_NO_SHADOW);
		bool hasBvh = frame.model != nullptr && (frame.model->GetBvhNodeBuffer() != nullptr || frame.model->HasMovingBvh());
		if (frame.tracedShadows && frame.shadows && hasBvh)
		{
			glm::vec3 eye(glm::inverse(frame.camera.view)[3]);
			std::vector<std::pair<float, uint32_t>> candidates;
			for (uint32_t lightIt = 0; lightIt < frame.lights.size(); lightIt++)
			{
				const auto& light = frame.lights[lightIt];
				float luminance = (0.2126F * light.intensity[0]) + (0.7152F * light.intensity[1]) + (0.0722F * light.intensity[2]);
				if (luminance <= 0.0F)
					continue;
				if (light.type == LIGHT_TYPE_DIRECTIONAL)
					candidates.emplace_back(std::numeric_limits<float>::max() / 2.0F + luminance, lightIt);
				else if (myShadowPlan.lightShadows[lightIt] != SHADER_TYPES_NO_SHADOW)
				{
					glm::vec3 position(light.positionRange[0], light.positionRange[1], light.positionRange[2]);
					candidates.emplace_back(luminance / std::max(glm::dot(position - eye, position - eye), 1e-6F), lightIt);
				}
			}
			std::ranges::sort(candidates, std::greater{});
			for (size_t slot = 0; slot < std::min<size_t>(candidates.size(), SHADER_TYPES_SHADOW_TRACE_SLOT_COUNT); slot++)
				trace.lights[slot] = candidates[slot].second;
			trace.movingBvh = frame.model->HasMovingBvh() ? 1U : 0U;
			trace.mapped = frame.tracedShadowsEverywhere ? 0U : 1U;
		}
		myShadowsTraced = trace.lights[0] != SHADER_TYPES_NO_SHADOW;
		auto& buffer = myShadowTraceBuffers[frame.frameIndex];
		auto memory = buffer.Map();
		std::memcpy(memory.data(), &trace, sizeof(trace));
		buffer.Flush(0, sizeof(trace));
		buffer.Unmap();
		for (auto pass : {myShadowTraceClearPass, myShadowClassifyPass, myShadowTracePass, myShadowDenoiseXPass, myShadowDenoiseYPass})
			myGraph.SetEnabled(pass, myShadowsTraced);
	}
	// the emissive surfaces as lights (see EmitterData), where the model has them and BVHs to trace their shadow rays
	// through; the last frame's reservoirs and history count if they lit it (at this size: Resize clears them)
	{
		bool hasBvh = frame.model != nullptr && (frame.model->GetBvhNodeBuffer() != nullptr || frame.model->HasMovingBvh());
		uint32_t emitterCount = frame.emissiveLights && hasBvh ? frame.model->GetEmitterCount() : 0;
		myEmittersOn = emitterCount > 0;
		for (auto pass : {myEmitterSamplePass, myEmitterShadePass, myEmitterXPass, myEmitterYPass, myEmitterTemporalPass})
			myGraph.SetEnabled(pass, myEmittersOn);
		bool history = myEmitterHistoryValid && myEmittersOn;
		EmitterData data{};
		data.count = emitterCount;
		data.candidateCount = myEmitterCandidateCount;
		data.voxelCandidateCount = myEmitterVoxelCandidateCount;
		data.voxelSampleCount = myEmitterVoxelSampleCount;
		data.frame = myEmitterFrame;
		data.temporalReuse = history && myEmitterReuse ? 1U : 0U;
		data.historyCap = myEmitterHistoryCap;
		data.spatialCount = myEmitterReuse ? myEmitterSpatialCount : 0U;
		data.spatialRadius = myEmitterSpatialRadius;
		data.blend = history ? myEmitterBlend : 1.0F;
		data.movingBvh = myEmittersOn && frame.model->HasMovingBvh() ? 1U : 0U;
		data.filterReach = myEmitterFilterReach;
		data.stride = myEmitterStride;
		data.roulette = myEmitterRoulette;
		// fewer where something moves, which may come between a point and the pixel
		data.visibilityFrameCount = data.movingBvh != 0U ? std::max(myEmitterVisibilityFrameCount / 4, 1U) : myEmitterVisibilityFrameCount;
		data.raw = frame.pushConstants.debugView == DEBUG_VIEW_EMITTERS_RAW ? 1U : (frame.pushConstants.debugView == DEBUG_VIEW_EMITTERS_REFERENCE ? 2U : 0U);
		// the reference accumulates every frame it is shown, evenly (a still camera's: the history is reprojected)
		myEmitterReferenceFrameCount = data.raw == 2U && history ? myEmitterReferenceFrameCount + 1 : 0;
		if (data.raw == 2U)
			data.blend = 1.0F / static_cast<float>(myEmitterReferenceFrameCount + 1);
		auto& buffer = myEmitterDataBuffers[frame.frameIndex];
		auto memory = buffer.Map();
		std::memcpy(memory.data(), &data, sizeof(data));
		buffer.Flush(0, sizeof(data));
		buffer.Unmap();
		myEmitterHistoryValid = myEmittersOn;
		if (myEmittersOn)
			myEmitterFrame++;
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
		auto& statistics = myFrameStatisticsThisFrame;
		statistics.clusterLightIndexCount = static_cast<uint32_t>(plan.lights.size());
		uint64_t reachedLights = 0;
		uint32_t reachedClusters = 0;
		for (size_t rangeIt = 1; rangeIt < plan.ranges.size(); rangeIt += 2)
			if (uint32_t count = plan.ranges[rangeIt]; count > 0)
			{
				statistics.clusterMaxLightCount = std::max(statistics.clusterMaxLightCount, count);
				reachedLights += count;
				reachedClusters++;
			}
		statistics.clusterMeanLightCount =
			reachedClusters > 0 ? static_cast<float>(static_cast<double>(reachedLights) / reachedClusters) : 0.0F;
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
	frame.pushConstants.shadows = shadowsDrawn || myShadowsTraced ? 1U : 0U;
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
		std::erase_if(
			myRetiredGiVolumes, [this](const auto& retired) { return myFrameCount > retired.first + SHADER_TYPES_FRAME_COUNT; });
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
		// the pre-pass's: the main list's, its opaque draws as the pre-pass's fragments, one pipeline a topology
		myPrepassList = myDrawLists[std::to_underlying(MainPassPhase::kOpaque)];
		for (auto& item : myPrepassList.items)
			if (item.variant.fragmentShader == 0)
				item.variant = {.topology = item.variant.topology, .blend = kOpaqueBlend, .fragmentShader = kPrepassFragmentShader};
		if (!records.empty())
		{
			auto bytes = std::as_bytes(std::span(records));
			auto memory = buffer.Map();
			std::ranges::copy(bytes, memory.begin());
			buffer.Flush(0, bytes.size());
			buffer.Unmap();
		}
	}
	// what the frame draws, for the Statistics window
	{
		static_assert(FrameStatistics::kListNames.size() == std::to_underlying(ListId::kCount));
		auto& statistics = myFrameStatisticsThisFrame;
		for (uint8_t listIt = 0; listIt < std::to_underlying(ListId::kCount); listIt++)
		{
			const auto& list = InternalList(static_cast<ListId>(listIt));
			statistics.listDraws[listIt] = static_cast<uint32_t>(list.items.size());
			for (const auto& item : list.items)
				statistics.listTriangles[listIt] += static_cast<uint64_t>(item.indexCount / 3) * item.instanceCount;
		}
		statistics.shadowViewCount = static_cast<uint32_t>(myShadowPlan.views.size());
		statistics.shadowedLightCount = static_cast<uint32_t>(std::ranges::count_if(
			std::span(myShadowPlan.lightShadows).first(std::min(frame.lights.size(), myShadowPlan.lightShadows.size())),
			[](uint32_t first) { return first != SHADER_TYPES_NO_SHADOW; }));
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

	myGraph.Bind(myOutput, myRenderOutput.GetTarget(frame.outputIndex), 0);
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
	myGraph.Bind(myGiStatisticsId, myGiStatisticsBuffers[frame.frameIndex]);
	InternalPlanGi(pipeline, frame);
	myFrameStatisticsThisFrame.occlusion = myOcclusionThisFrame;
	myFrameStatisticsThisFrame.shadowOcclusion = myShadowOcclusionThisFrame;
	myFrameStatistics.Write().Get() = myFrameStatisticsThisFrame;
	myGraph.Bind(myGiVoxels, myGiVoxelBuffer);
	myGraph.Bind(myGiRadiance, myGiRadianceBuffer);
	myGraph.Bind(myGiRays, myGiRayBuffer);
	myGraph.Bind(myGiIrradiance, myGiIrradianceBuffer);
	myGraph.Bind(myGiProbeDepth, myGiProbeDepthBuffer);
	myGraph.Bind(myGiFlags, myGiFlagBuffer);
	myGraph.Bind(myGiVisibility, myGiVisibilityBuffer);
	myGraph.Bind(myGiCache, myGiCacheBuffer);
	myGraph.Bind(myGiCacheRecords, myGiCacheRecordBuffer);
	myGraph.Bind(myGiCacheTables, myGiCacheTableBuffer);
	myGraph.Bind(myGiCacheFree, myGiCacheFreeBuffer);
	for (uint32_t volumeIt = 0; volumeIt < SHADER_TYPES_GI_VOLUME_COUNT; volumeIt++)
		myGraph.Bind(myGiVolumes[volumeIt], *myGiVolumeImages[volumeIt].image);
	for (uint32_t historyIt = 0; historyIt < 2; historyIt++)
	{
		myGraph.Bind(myGiNearHistory[historyIt], *myGiNearHistoryImages[historyIt].image);
		myGraph.Bind(myGiNearHistoryDistance[historyIt], *myGiNearHistoryDistanceImages[historyIt].image);
		myGraph.Bind(myEmitterHistory[historyIt], *myEmitterHistoryImages[historyIt].image);
		myGraph.Bind(myEmitterSurfaces[historyIt], *myEmitterSurfaceImages[historyIt].image);
	}
	myGraph.Bind(myGiStaticVoxels, myGiStaticVoxelBuffer);
	myGraph.Bind(myEmitterReservoirs, myEmitterReservoirBuffer);

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

bool Renderer::InternalCullsOnGpu(ListId /*list*/, const FrameInputs& /*frame*/) const noexcept
{
	return myGpuCulling;
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
	myCullShadowLateMaxCandidateCount = 0;
	myCullShadowLateJobBase = 0;
	myCullShadowLateJobCount = 0;
	myCullMainLateJobBase = 0;
	const auto& mainList = InternalList(ListId::kMain);
	const auto& shadowList = InternalList(ListId::kShadow);
	myOcclusionThisFrame = myOcclusionCulling && frame.culling && !mainList.Empty();
	myShadowOcclusionThisFrame = myShadowOcclusionCulling && frame.culling && !shadowList.Empty() && !myShadowPlan.views.empty();
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
			  myDepthSavePass,
			  myShadowDepthPyramidPass,
			  myCullShadowsLatePass,
			  myShadowsLatePass})
			myGraph.SetEnabled(pass, false);
		myPreviousValid = false;
		myShadowOcclusionThisFrame = false;
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
			views[viewIt].pyramid = 0;
			views[viewIt].maxLevel = SHADER_TYPES_DEPTH_PYRAMID_LEVEL_COUNT;
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
		// the shadow views in their tiles of the atlas, occluded by its pyramid: a tile of size s is one texel at level
		// log2(s) - 1 (level 0 is half the atlas), aligned (see shadows::Plan), so no coarser level is read
		for (size_t viewIt = 0; viewIt < myShadowPlan.views.size(); viewIt++)
		{
			auto& view = views[mainViewCount + viewIt];
			setView(mainViewCount + viewIt, glm::make_mat4(&myShadowPlan.views[viewIt].viewProjection[0][0]));
			const auto& tile = myShadowPlan.tiles[viewIt];
			ENSURE(std::has_single_bit(tile.size) && tile.x % tile.size == 0 && tile.y % tile.size == 0);
			view.viewport[0] = static_cast<float>(tile.x);
			view.viewport[1] = static_cast<float>(tile.y);
			view.viewport[2] = static_cast<float>(tile.size);
			view.viewport[3] = static_cast<float>(tile.size);
			view.occlusion = myShadowOcclusionThisFrame ? 1U : 0U;
			view.pyramid = 1;
			view.maxLevel = static_cast<uint32_t>(std::bit_width(tile.size)) - 2;
		}
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
	// the visibility per draw and view, kept from frame to frame: the main views', first, so that its place holds as long
	// as the main list's draws and the views do, then the shadow views'
	bool mainVisibility = myOcclusionThisFrame && myOcclusionMode != OcclusionMode::kReprojected;
	auto shadowVisibilityBase = mainVisibility ? static_cast<uint32_t>(mainList.items.size()) * mainViewCount : 0U;
	auto visibilityCount =
		shadowVisibilityBase +
		(myShadowOcclusionThisFrame ? static_cast<uint32_t>(shadowList.items.size() * myShadowPlan.views.size()) : 0U);
	// a list's jobs, a job per view, of the Cull pass (before the shadows and the main pass), the Cull Shadows Late pass or
	// the Cull Late pass (after the main pass)
	enum class JobSet : uint8_t
	{
		kEarly,
		kShadowLate,
		kMainLate,
	};
	auto addJobs = [&](ListId listId, uint32_t kind, uint32_t visibilityBase, JobSet set = JobSet::kEarly)
	{
		bool late = set != JobSet::kEarly;
		auto& maxCandidateCount = set == JobSet::kEarly		  ? myCullMaxCandidateCount
								  : set == JobSet::kShadowLate ? myCullShadowLateMaxCandidateCount
															   : myCullLateMaxCandidateCount;
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
				.compact = myCountEmulated ? 0U : 1U,
				.statistics = shadow ? CULL_STATISTIC_SHADOW : CULL_STATISTIC_MAIN});
			commandCount += itemCount;
			myCullCountCount += static_cast<uint32_t>(list.runs.size());
		}
		maxCandidateCount = std::max(maxCandidateCount, itemCount);
	};
	auto onGpu = [&](ListId listId) { return !InternalList(listId).Empty() && InternalCullsOnGpu(listId, frame); };
	for (uint8_t listIt = 0; listIt < std::to_underlying(ListId::kCount); listIt++)
	{
		auto listId = static_cast<ListId>(listIt);
		if (!onGpu(listId))
			continue;
		// what the culling tests: each draw once per view it is drawn in
		{
			bool shadow = listId == ListId::kShadow;
			auto viewCount = shadow ? myShadowPlan.views.size() : mainViewCount;
			auto group = shadow ? CULL_STATISTIC_SHADOW : CULL_STATISTIC_MAIN;
			const auto& list = InternalList(listId);
			myFrameStatisticsThisFrame.testedDraws[group] += list.items.size() * viewCount;
			for (const auto& item : list.items)
				myFrameStatisticsThisFrame.testedTriangles[group] += static_cast<uint64_t>(item.indexCount / 3) * item.instanceCount * viewCount;
		}
		if (myOcclusionThisFrame && listId == ListId::kMain)
		{
			// the main pass's first draws: two-phase, those seen last frame, by their visibility (kept from frame to frame:
			// the main list's comes first, so its place stays as long as its draws and the views do); reprojected, all it
			// draws, by the last frame's depth; hybrid, those that doesn't hide (marked in the visibility)
			switch (myOcclusionMode)
			{
			case OcclusionMode::kTwoPhase: addJobs(listId, CULL_JOB_EARLY, 0); break;
			case OcclusionMode::kReprojected: addJobs(listId, CULL_JOB_PYRAMID, 0); break;
			case OcclusionMode::kHybrid: addJobs(listId, CULL_JOB_HYBRID_EARLY, 0); break;
			}
			continue;
		}
		if (myShadowOcclusionThisFrame && listId == ListId::kShadow)
		{
			// the casters each shadow view saw last frame
			addJobs(listId, CULL_JOB_EARLY, shadowVisibilityBase);
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
	myCullShadowLateJobBase = myCullEarlyJobCount;
	if (myShadowOcclusionThisFrame)
		addJobs(ListId::kShadow, CULL_JOB_LATE, shadowVisibilityBase, JobSet::kShadowLate);
	myCullShadowLateJobCount = static_cast<uint32_t>(myCullJobs.size()) - myCullShadowLateJobBase;
	myCullMainLateJobBase = static_cast<uint32_t>(myCullJobs.size());
	bool reprojected = myOcclusionThisFrame && myOcclusionMode != OcclusionMode::kTwoPhase;
	bool lateDraws = myOcclusionThisFrame && myOcclusionMode != OcclusionMode::kReprojected;
	if (lateDraws)
	{
		addJobs(ListId::kMain, myOcclusionMode == OcclusionMode::kHybrid ? CULL_JOB_HYBRID_LATE : CULL_JOB_LATE, 0, JobSet::kMainLate);
		if (onGpu(ListId::kMainTransmissive))
			addJobs(ListId::kMainTransmissive, CULL_JOB_PYRAMID, 0, JobSet::kMainLate);
	}
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
	myGraph.SetEnabled(myPrepassLatePass, lateDraws);
	myGraph.SetEnabled(myMainLatePass, lateDraws);
	myGraph.SetEnabled(myDepthSavePass, reprojected);
	myGraph.SetEnabled(myShadowDepthPyramidPass, myShadowOcclusionThisFrame);
	myGraph.SetEnabled(myCullShadowsLatePass, myShadowOcclusionThisFrame);
	myGraph.SetEnabled(myShadowsLatePass, myShadowOcclusionThisFrame);

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

void Renderer::InternalReplaceBuffer(
	Buffer& buffer, Pipeline& pipeline, std::string_view name, std::string_view binding, size_t size, BufferUsage usage)
{
	auto& device = pipeline.GetDevice();
	Buffer replacement(BufferCreateDesc{
		device.CreateDeviceObjectCreateDesc(std::string(name)), std::max<size_t>(size, 16), usage, MemoryProperty::kDeviceLocal});
	if (!binding.empty())
		pipeline.SetDescriptorData(binding, BufferBinding{.buffer = replacement, .offset = 0}, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES);
	myRetiredBuffers.emplace_back(myFrameCount, std::exchange(buffer, std::move(replacement)));
}

void Renderer::InternalReplaceGiVolumes(Pipeline& pipeline, const glm::uvec3& extent)
{
	auto& device = pipeline.GetDevice();
	for (uint32_t volumeIt = 0; volumeIt < SHADER_TYPES_GI_VOLUME_COUNT; volumeIt++)
	{
		GiVolume volume;
		ImageCreateDesc desc{device.CreateDeviceObjectCreateDesc(std::format("GI Volume {}", volumeIt))};
		desc.mipLevels = {{.extent = {.width = extent.x, .height = extent.y}}};
		desc.format = Format::kR16G16B16A16Sfloat;
		desc.tiling = ImageTiling::kOptimal;
		desc.usageFlags = ImageUsage::kStorage | ImageUsage::kSampled | ImageUsage::kTransferDestination; // cleared by GI Clear
		desc.memoryFlags = MemoryProperty::kDeviceLocal;
		desc.imageAspectFlags = ImageAspect::kColor;
		desc.layout = ImageLayout::kUndefined;
		desc.depth = extent.z;
		volume.image = std::make_unique<Image>(std::move(desc));
		ImageViewCreateDesc viewDesc{
			device.CreateDeviceObjectCreateDesc(std::format("GI Volume {} View", volumeIt)), *volume.image, Format::kR16G16B16A16Sfloat, ImageAspect::kColor};
		viewDesc.volume = true;
		volume.view = std::make_unique<ImageView>(std::move(viewDesc));
		pipeline.SetDescriptorData(
			"gGiVolumes",
			ImageBinding{.sampler = {}, .imageView = *volume.view, .layout = ImageLayout::kGeneral},
			DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES,
			volumeIt);
		pipeline.SetDescriptorData(
			"gGiVolumeImages",
			ImageBinding{.sampler = {}, .imageView = *volume.view, .layout = ImageLayout::kGeneral},
			DESCRIPTOR_SET_CATEGORY_GLOBAL_RW_TEXTURES,
			volumeIt);
		if (myGiVolumeImages[volumeIt].image)
			myRetiredGiVolumes.emplace_back(myFrameCount, std::exchange(myGiVolumeImages[volumeIt], std::move(volume)));
		else
			myGiVolumeImages[volumeIt] = std::move(volume);
	}
}

void Renderer::InternalPlanGi(Pipeline& pipeline, const FrameInputs& frame)
{
	ZoneScopedN("Renderer::planGi");

	using namespace renderer;

	myGiSurfaceCacheOn = frame.indirectLighting && frame.giSurfaceCache;

	const auto& mainList = InternalList(ListId::kMain);
	bool enabled = frame.indirectLighting && frame.model != nullptr && !mainList.Empty();
	auto write = [](Buffer& buffer, std::span<const std::byte> bytes)
	{
		if (bytes.empty())
			return;
		auto memory = buffer.Map();
		std::ranges::copy(bytes, memory.begin());
		buffer.Flush(0, bytes.size());
		buffer.Unmap();
	};
	for (auto pass : {myGiClearPass, myGiShiftPass, myGiVoxelizePass, myGiKeepPass, myGiLightStaticPass, myGiRestorePass, myGiVoxelizeMovingPass, myGiCacheGrowPass, myGiPass})
		myGraph.SetEnabled(pass, enabled);
	// the near field (see GiData::nearReach), where the rays trace the model's BVHs
	bool hasBvh = frame.model != nullptr && (frame.model->GetBvhNodeBuffer() != nullptr || frame.model->HasMovingBvh());
	myGiNearOn = enabled && frame.giNearField && frame.traceTriangles && hasBvh && myGiNearReach > 0.0F;
	for (auto pass : {myGiNearPass, myGiNearXPass, myGiNearYPass})
		myGraph.SetEnabled(pass, myGiNearOn);
	// and over frames: last frame's history counts if the frames are on and it ran then (at this size: Resize clears it),
	// else this frame's rays alone start it over. the pass runs with the near field whether or not (the frame's alone:
	// blended by 1), since the main pass upsamples its traced pixels' surfaces
	myGiNearTemporalOn = myGiNearOn && frame.giNearTemporal;
	myGraph.SetEnabled(myGiNearTemporalPass, myGiNearOn);
	myGiNearFrameBlend = myGiNearHistoryValid && myGiNearTemporalOn ? myGiNearBlend : 1.0F;
	myGiNearHistoryValid = myGiNearTemporalOn;
	if (myGiNearOn)
		myGiNearFrame++;
	if (!enabled)
	{
		std::array<GiData, SHADER_TYPES_GI_CLIP_COUNT> data{};
		write(myGiDataBuffers[frame.frameIndex], std::as_bytes(std::span(data)));
		myGiModel = nullptr;
		return;
	}
	// the static voxels are lit again when the lights change (any of them: the environment's rotation turns its dominant
	// lights, which are among them), and when the emissive surfaces start or stop lighting
	myGiEmittersToggled = myGiEmittersLit != myEmittersOn;
	myGiLightsChanged = frame.lights.size() != myGiLights.size() || !std::ranges::equal(std::as_bytes(frame.lights), std::as_bytes(std::span(myGiLights))) ||
						myGiEmittersLit != myEmittersOn;
	// a change is gradual where the same lights only turned and dimmed a little (the sky's sun as the time of day
	// advances, drawn again every 0.05 degrees): the surface cache then keeps what it has and follows them, sampling as
	// where something moves (at most 32 samples, see GiCacheUpdate), for kGiLightsFollowFrameCount frames after the
	// last change, rather than starting over every frame (one ray a voxel, never accumulated)
	myGiLightsGradual = myGiLightsChanged && frame.lights.size() == myGiLights.size() && myGiEmittersLit == myEmittersOn &&
						std::ranges::all_of(
							std::views::iota(size_t{0}, frame.lights.size()),
							[&frame, this](size_t lightIt)
							{
								constexpr float kTurnDegrees = 2.0F;
								constexpr float kDimRatio = 1.25F;
								const auto& now = frame.lights[lightIt];
								const auto& last = myGiLights[lightIt];
								auto luminance = [](const LightData& light)
								{ return (0.2126F * light.intensity[0]) + (0.7152F * light.intensity[1]) + (0.0722F * light.intensity[2]); };
								float ratio = (luminance(now) + 1e-6F) / (luminance(last) + 1e-6F);
								return now.type == last.type && std::ranges::equal(now.positionRange, last.positionRange) && now.size == last.size &&
									   glm::dot(glm::make_vec3(now.direction), glm::make_vec3(last.direction)) >= std::cos(glm::radians(kTurnDegrees)) &&
									   ratio <= kDimRatio && ratio >= 1.0F / kDimRatio;
							});
	if (myGiLightsGradual)
		myGiLightsFollowFrames = kGiLightsFollowFrameCount;
	else if (myGiLightsChanged)
		myGiLightsFollowFrames = 0;
	else if (myGiLightsFollowFrames > 0)
		myGiLightsFollowFrames--;
	if (myGiLightsChanged)
		myGiLights.assign(frame.lights.begin(), frame.lights.end());
	myGiEmittersLit = myEmittersOn;

	// the clipmap, made again (and voxelized) when the model, its bounds or its draws change; a clip that moves with the
	// camera is voxelized again
	glm::mat4 cameraToWorld = glm::inverse(frame.camera.view);
	glm::vec3 eye = cameraToWorld[3];
	// toward what the camera looks at (the view's -z, as far as the last frame's pixels were, see FrameInputs::lookDistance)
	glm::vec3 look = -glm::normalize(glm::vec3(cameraToWorld[2])) * std::max(frame.lookDistance, 0.0F);
	if (!myGiEyePath.empty())
	{
		const auto& point = myGiEyePath[std::min(myGiEyePathNext++, myGiEyePath.size() - 1)];
		eye = point.eye;
		look = point.look.value_or(glm::vec3(0.0F));
	}
	if (myGiEyeLog)
		std::println("gi eye {},{},{},{},{},{}", eye.x, eye.y, eye.z, look.x, look.y, look.z);
	// another resolution: made again, as for another scene
	auto resolution = std::clamp<uint32_t>(frame.giResolution, gi::kMinResolution, gi::kMaxResolution);
	bool resolutionChanged = resolution != myGiResolution;
	myGiResolution = resolution;
	auto clipmap = gi::MakeClipmap(frame.sceneBounds, eye, look, myGiLookAhead, myGiResolution, myGiClipCount, myGiProbeSpacing);
	bool changed = frame.model != myGiModel || frame.sceneBounds.GetMin() != myGiBounds.GetMin() ||
				   frame.sceneBounds.GetMax() != myGiBounds.GetMax() || mainList.items.size() != myGiItemCount || myGiRevoxelize || resolutionChanged;
	// how far each clip moved, in voxels (whole probe cells, see gi::MakeClipmap): a voxel takes the one that was at its
	// place plus that. all of it is voxelized again when the scene changed, or when it moved by its size or more
	for (uint32_t clipIt = 0; clipIt < clipmap.clips.size(); clipIt++)
	{
		auto& jobs = myGiClipJobs[clipIt];
		const auto& data = clipmap.clips[clipIt].data;
		glm::ivec3 size(data.volumeSize[0], data.volumeSize[1], data.volumeSize[2]);
		jobs.scroll = size;
		if (!changed && clipIt < myGiClipmap.clips.size())
		{
			const auto& before = myGiClipmap.clips[clipIt].data.volumeOrigin;
			glm::vec3 moved = (glm::vec3(data.volumeOrigin[0], data.volumeOrigin[1], data.volumeOrigin[2]) - glm::vec3(before[0], before[1], before[2])) /
							  data.volumeOrigin[3];
			auto scroll = glm::ivec3(glm::round(moved));
			if (glm::all(glm::lessThan(glm::abs(scroll), size)))
				jobs.scroll = scroll;
		}
		jobs.voxelize = jobs.scroll != glm::ivec3(0);
	}
	if (changed)
	{
		myGiModel = frame.model;
		myGiBounds = frame.sceneBounds;
		myGiItemCount = mainList.items.size();
		const auto& total = clipmap.total;
		const auto& previous = myGiClipmap.total;
		if (total.voxelCount != previous.voxelCount)
		{
			constexpr auto kVoxelUsage = BufferUsage::kStorage | BufferUsage::kTransferSource | BufferUsage::kTransferDestination;
			InternalReplaceBuffer(myGiVoxelBuffer, pipeline, "GI Voxels", "gGiVoxels", total.voxelCount * sizeof(GiVoxel), kVoxelUsage);
			InternalReplaceBuffer(myGiStaticVoxelBuffer, pipeline, "GI Static Voxels", "gGiStaticVoxels", total.voxelCount * sizeof(GiVoxel), kVoxelUsage);
			// which probes each voxel sees, in two halves as the probes (see GiData::visibilityFirst)
			InternalReplaceBuffer(
				myGiVisibilityBuffer, pipeline, "GI Visibility", "gGiVisibility", 2 * static_cast<uint64_t>(total.voxelCount) * sizeof(uint32_t), kVoxelUsage);
			const auto& size = clipmap.clips.front().data.volumeSize;
			InternalReplaceGiVolumes(
				pipeline, glm::uvec3(clipmap.clips.back().data.volumeImage[0], clipmap.clips.back().data.volumeImage[1], clipmap.clips.back().data.volumeImage[2]));
		}
		if (total.radianceCount != previous.radianceCount)
			InternalReplaceBuffer(myGiRadianceBuffer, pipeline, "GI Radiance", "gGiRadiance", total.radianceCount * 2 * sizeof(uint32_t));
		if (total.flagCount != previous.flagCount)
			InternalReplaceBuffer(myGiFlagBuffer, pipeline, "GI Flags", "gGiFlags", total.flagCount * SHADER_TYPES_GI_FLAG_WORD_COUNT * sizeof(uint32_t));
		// the probes' buffers in two halves (see GiData::probeFirst)
		if (total.probeCount != previous.probeCount)
		{
			InternalReplaceBuffer(myGiRayBuffer, pipeline, "GI Rays", "gGiRays", static_cast<uint64_t>(total.probeCount) * myGiRayCount * 2 * sizeof(uint32_t));
			InternalReplaceBuffer(
				myGiIrradianceBuffer, pipeline, "GI Irradiance", "gGiIrradiance", static_cast<uint64_t>(total.probeCount) * 6 * SHADER_TYPES_GI_IRRADIANCE_COUNT * 4 * sizeof(float));
			InternalReplaceBuffer(
				myGiProbeDepthBuffer,
				pipeline,
				"GI Probe Depth",
				"gGiProbeDepth",
				static_cast<uint64_t>(total.probeCount) * 2 * SHADER_TYPES_GI_DEPTH_SIDE * SHADER_TYPES_GI_DEPTH_SIDE * sizeof(uint32_t));
		}
		// nothing of the last frame's probes is kept
		myGiReset = true;
		myGiClearBuffers = true;
	}
	ENSUREF(
		clipmap.total.voxelCount == myGiClipmap.total.voxelCount || changed,
		"the indirect lighting's clips changed their sizes without the scene changing");
	myGiClipmap = std::move(clipmap);
	// the surface cache's storage (see GiCachePopPage), only while it is on: a page for each block that holds entries and
	// was read lately, from a pool that starts at a thirty-second of the clipmap's blocks (all those near a surface were
	// three quarters of them at resolution 128 in Sponza's corridor, a third at 384) and grows to a quarter more than the
	// resolve counted wanting one (GI_STATISTIC_CACHE_BLOCKS), its pages copied over (the GI Cache Grow pass); the
	// filter's records likewise, for the blocks a lookup read (GI_STATISTIC_CACHE_ACTIVE_BLOCKS), from a sixty-fourth of
	// the clipmap's blocks, made anew (they last a frame). made for another clipmap, or when the cache is turned on, it
	// starts over (GiData::cacheInit). an entry for every voxel of every clip, as it was, took 6.4 GB at resolution 384;
	// a page for every block near a surface 1.1 GB; this, 0.3 GB
	if (myGiCacheGrowPages.IsValid())
	{
		myRetiredBuffers.emplace_back(myFrameCount, std::move(myGiCacheGrowPages));
		myRetiredBuffers.emplace_back(myFrameCount, std::move(myGiCacheGrowFree));
	}
	myGiCacheInit = false;
	myGiCacheGrowFirst = 0;
	{
		constexpr auto kCacheUsage = BufferUsage::kStorage | BufferUsage::kTransferSource | BufferUsage::kTransferDestination;
		auto capacityFor = [](uint64_t count)
		{ return static_cast<uint32_t>((count * 5 / 4 + kGiCachePageStep) / kGiCachePageStep * kGiCachePageStep); };
		auto replace = [&](Buffer& buffer, std::string_view name, std::string_view binding, uint64_t size)
		{ InternalReplaceBuffer(buffer, pipeline, name, binding, size, kCacheUsage); };
		uint32_t blockCount = myGiClipmap.total.flagCount;
		if (!frame.giSurfaceCache)
		{
			if (myGiCachePageCapacity != 0)
			{
				replace(myGiCacheBuffer, "GI Cache", "gGiCache", 3 * 4 * sizeof(uint32_t));
				replace(myGiCacheRecordBuffer, "GI Cache Records", "gGiCacheRecords", 3 * 4 * sizeof(uint32_t));
				replace(myGiCacheTableBuffer, "GI Cache Tables", "gGiCacheBlocks", sizeof(uint32_t));
				replace(myGiCacheFreeBuffer, "GI Cache Free", "gGiCacheFree", 2 * sizeof(uint32_t));
				myGiCachePageCapacity = 0;
				myGiCacheRecordCapacity = 0;
				myGiCacheBlockCount = 0;
			}
		}
		else if (myGiCachePageCapacity == 0 || changed || blockCount != myGiCacheBlockCount)
		{
			myGiCachePageCapacity = capacityFor(blockCount / 32);
			myGiCacheRecordCapacity = capacityFor(blockCount / 64);
			myGiCacheBlockCount = blockCount;
			myGiCacheBlocksCounted = 0;
			myGiCacheActiveBlocksCounted = 0;
			replace(myGiCacheBuffer, "GI Cache", "gGiCache", myGiCachePageCapacity * kGiCachePageSize);
			replace(myGiCacheRecordBuffer, "GI Cache Records", "gGiCacheRecords", 2 * myGiCacheRecordCapacity * kGiCachePageSize);
			// a frame's page table and the last frame's (two words a block), then the records'
			replace(myGiCacheTableBuffer, "GI Cache Tables", "gGiCacheBlocks", 5 * static_cast<uint64_t>(blockCount) * sizeof(uint32_t));
			replace(myGiCacheFreeBuffer, "GI Cache Free", "gGiCacheFree", (2 + static_cast<uint64_t>(myGiCachePageCapacity)) * sizeof(uint32_t));
			myGiCacheInit = true;
		}
		else
		{
			if (myGiCacheBlocksCounted > myGiCachePageCapacity)
			{
				// the new pool and free list, the old ones kept for this frame's copy (then retired)
				auto grow = [&](Buffer& buffer, Buffer& from, std::string_view name, std::string_view binding, uint64_t size)
				{
					auto& device = pipeline.GetDevice();
					Buffer replacement(
						BufferCreateDesc{device.CreateDeviceObjectCreateDesc(std::string(name)), size, kCacheUsage, MemoryProperty::kDeviceLocal});
					pipeline.SetDescriptorData(binding, BufferBinding{.buffer = replacement, .offset = 0}, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES);
					from = std::exchange(buffer, std::move(replacement));
				};
				uint32_t capacity = capacityFor(myGiCacheBlocksCounted);
				grow(myGiCacheBuffer, myGiCacheGrowPages, "GI Cache", "gGiCache", capacity * kGiCachePageSize);
				grow(myGiCacheFreeBuffer, myGiCacheGrowFree, "GI Cache Free", "gGiCacheFree", (2 + static_cast<uint64_t>(capacity)) * sizeof(uint32_t));
				myGiCacheGrowFirst = myGiCachePageCapacity;
				myGiCachePageCapacity = capacity;
			}
			if (myGiCacheActiveBlocksCounted > myGiCacheRecordCapacity)
			{
				myGiCacheRecordCapacity = capacityFor(myGiCacheActiveBlocksCounted);
				replace(myGiCacheRecordBuffer, "GI Cache Records", "gGiCacheRecords", 2 * myGiCacheRecordCapacity * kGiCachePageSize);
			}
		}
		myGiCachePrepareThreadCount = std::max(myGiCacheInit ? 5 * myGiCacheBlockCount : 0U, myGiCachePageCapacity);
		myGraph.SetEnabled(myGiCacheGrowPass, myGiCacheGrowFirst != 0);
	}
	bool voxelize = std::ranges::any_of(myGiClipJobs, [](const auto& jobs) { return jobs.voxelize; });

	// what the voxelizer reads, when it runs: the main list's triangles of what doesn't move (opaque or alpha masked), a
	// job per draw, then the triangle count
	myGraph.SetEnabled(myGiClearPass, std::exchange(myGiClearBuffers, false));
	myGraph.SetEnabled(myGiShiftPass, voxelize);
	myGraph.SetEnabled(myGiVoxelizePass, voxelize);
	myGraph.SetEnabled(myGiKeepPass, voxelize);
	myGraph.SetEnabled(myGiLightStaticPass, voxelize || myGiLightsChanged);
	{
		// a job per draw (its first triangle of the run, first index, DrawRecord and instances), the triangle count after:
		// per clip voxelized, the static draws whose bounds reach it, then what moves, every frame (for every clip)
		std::vector<uint32_t> jobs;
		auto addJobs = [&jobs, &mainList](bool moving, std::span<const Bounds3f> reach, uint32_t& jobCount, uint32_t& triangleCount)
		{
			auto first = jobs.size();
			uint32_t triangles = 0;
			for (const auto& item : mainList.items)
			{
				if (item.moves != moving || item.variant.topology != PrimitiveTopology::kTriangleList || item.variant.fragmentShader != 0)
					continue;
				if (!moving && std::ranges::none_of(
								   reach,
								   [&item](const Bounds3f& box)
								   {
									   return !glm::any(glm::greaterThan(item.bounds.GetMin(), box.GetMax())) &&
											  !glm::any(glm::lessThan(item.bounds.GetMax(), box.GetMin()));
								   }))
					continue;
				// its instances, whether it moves (bit 31) and whether it is double sided (bit 30: see ComputeGiVoxelize)
				ENSUREF(item.instanceCount < 0x40000000U, "GI job instance count {} past 2^30", item.instanceCount);
				jobs.insert(
					jobs.end(),
					{triangles,
					 item.firstIndex,
					 item.record,
					 item.instanceCount | (moving ? 0x80000000U : 0U) | (item.cullMode == CullMode::kNone ? 0x40000000U : 0U)});
				triangles += item.indexCount / 3;
			}
			jobCount = static_cast<uint32_t>((jobs.size() - first) / 4);
			triangleCount = triangles;
			jobs.insert(jobs.end(), {triangles, 0U, 0U, 0U});
		};
		for (uint32_t clipIt = 0; clipIt < myGiClipmap.clips.size(); clipIt++)
		{
			auto& clipJobs = myGiClipJobs[clipIt];
			if (!clipJobs.voxelize)
				continue;
			// the slabs that scrolled in, per axis it moved along (all of it, when voxelized again)
			const auto& data = myGiClipmap.clips[clipIt].data;
			float voxelSize = data.volumeOrigin[3];
			glm::vec3 origin(data.volumeOrigin[0], data.volumeOrigin[1], data.volumeOrigin[2]);
			glm::ivec3 size(data.volumeSize[0], data.volumeSize[1], data.volumeSize[2]);
			std::vector<Bounds3f> slabs;
			for (int axisIt = 0; axisIt < 3; axisIt++)
			{
				int scroll = clipJobs.scroll[axisIt];
				if (scroll == 0)
					continue;
				glm::ivec3 low(0);
				glm::ivec3 high = size;
				if (scroll > 0)
					low[axisIt] = std::max(size[axisIt] - scroll, 0);
				else
					high[axisIt] = std::min(-scroll, size[axisIt]);
				slabs.emplace_back(origin + glm::vec3(low) * voxelSize - glm::vec3(voxelSize), origin + glm::vec3(high) * voxelSize + glm::vec3(voxelSize));
			}
			clipJobs.base = static_cast<uint32_t>(jobs.size() / 4);
			addJobs(false, slabs, clipJobs.count, clipJobs.triangleCount);
		}
		myGiMovingJobBase = static_cast<uint32_t>(jobs.size() / 4);
		addJobs(true, {}, myGiMovingJobCount, myGiMovingTriangleCount);
		write(InternalReserve(myGiJobs, pipeline, frame.frameIndex, jobs.size() / 4), std::as_bytes(std::span(jobs)));
		myGraph.SetEnabled(myGiRestorePass, myGiMovingJobCount > 0);
		myGraph.SetEnabled(myGiVoxelizeMovingPass, myGiMovingJobCount > 0);
	}

	// the lights per cell of each clip, and the parameters: the probes' halves (this frame's and the last's: swapped each
	// frame), the rays' subset of the frame (see GiData::rayPhase), and the blending's
	std::array<GiData, SHADER_TYPES_GI_CLIP_COUNT> data{};
	myGiProbeHalf ^= 1;
	myGiRayPhase = (myGiRayPhase + 1) % myGiRayPhaseCount;
	myGiFramesSinceReset = myGiReset ? 0 : myGiFramesSinceReset + 1;
	// the frames' length, smoothed over about ten (a stall shouldn't move the blend), and the probes' blend by it (see
	// myGiTimeBased): a probe takes as long to follow a change at any frame rate (by frames, five times as long at San
	// Miguel's 12 frames a second as at 60)
	auto frameTime = std::chrono::steady_clock::now();
	if (myGiFrameTime)
	{
		double seconds = std::clamp(std::chrono::duration<double>(frameTime - *myGiFrameTime).count(), 1.0 / 480.0, 0.25);
		myGiFrameSeconds += (seconds - myGiFrameSeconds) * 0.1;
	}
	myGiFrameTime = frameTime;
	float hysteresis = myGiHysteresis;
	if (myGiTimeBased)
	{
		constexpr float kMinHysteresis = 0.8F;
		hysteresis = std::clamp(static_cast<float>(std::pow(static_cast<double>(myGiHysteresis), myGiFrameSeconds * 60.0)), std::min(kMinHysteresis, myGiHysteresis), 0.995F);
	}
	// the surface cache's window: at most half the frames since it started over (and at least 32), so that the samples of
	// its first frames, while the probes and the bounce build up (and overshoot), fall out as that ends rather than as 1
	// over the samples: by its whole window from the start, a mean of up to 1024 samples, Sponza's interior still darkened
	// 1200 frames after a cold start, where without the cache it settled by 600 (docs/performance-plan.md)
	bool cacheReset = (myGiLightsChanged && !myGiLightsGradual) || myGiCacheInit || myGiReset;
	myGiFramesSinceCacheReset = cacheReset ? 0 : myGiFramesSinceCacheReset + 1;
	uint32_t cacheSampleCount = myGiTimeBased ? std::clamp(myGiFramesSinceCacheReset / 2, std::min(32U, myGiCacheSampleCount), myGiCacheSampleCount) : myGiCacheSampleCount;
	// the rays per probe this frame: the budget over the probes traced last time (at least 16, at most the count, in
	// eights)
	myGiRayCountThisFrame = myGiRayCount;
	if (myGiActiveProbeCount > 0)
		myGiRayCountThisFrame = std::clamp((myGiRayBudget / myGiActiveProbeCount) & ~7U, std::min(16U, myGiRayCount), myGiRayCount);
	myGiFrameStamp = (myGiFrameStamp % 0xFFFFFF) + 1;
	constexpr uint32_t kDepthTexelCount = SHADER_TYPES_GI_DEPTH_SIDE * SHADER_TYPES_GI_DEPTH_SIDE;
	uint32_t probeTotal = myGiClipmap.total.probeCount;
	for (uint32_t clipIt = 0; clipIt < myGiClipmap.clips.size(); clipIt++)
	{
		auto& clip = data[clipIt];
		clip = myGiClipmap.clips[clipIt].data;
		for (int axisIt = 0; axisIt < 3; axisIt++)
			clip.scroll[axisIt] = myGiClipJobs[clipIt].scroll[axisIt];
		clip.rayPhase = myGiRayPhase;
		clip.rayPhaseCount = myGiRayPhaseCount;
		clip.frameStamp = myGiFrameStamp;
		uint32_t clipFirst = clip.probeFirst;
		clip.probeFirst = myGiProbeHalf * probeTotal + clipFirst;
		clip.probePreviousFirst = (1 - myGiProbeHalf) * probeTotal + clipFirst;
		clip.visibilityFirst = myGiProbeHalf * myGiClipmap.total.voxelCount + clip.voxelFirst;
		clip.visibilityPreviousFirst = (1 - myGiProbeHalf) * myGiClipmap.total.voxelCount + clip.voxelFirst;
		// the probes' cycle sums and means, past both halves of their irradiance (see GiData::cycleOffset)
		clip.cycleOffset = 2 * probeTotal;
		// the surface cache (FrameInputs::giSurfaceCache), dropped when the lights change (their light is in it)
		clip.cacheRayCount = frame.giSurfaceCache ? myGiCacheRayCount : 0U;
		// following lights that change gradually (see myGiLightsGradual) as where something moves
		clip.cacheSampleCount = myGiLightsFollowFrames > 0 ? std::min(cacheSampleCount, 32U) : cacheSampleCount;
		clip.cacheReset = (myGiLightsChanged && !myGiLightsGradual) || myGiCacheInit ? 1U : 0U;
		clip.cacheShow = myGiCacheShow ? 1U : 0U;
		clip.cacheFilterPassCount = myGiCacheFilterPassCount;
		// the cache's tables (see GiData::cacheTableFirst): the clip's blocks from its first flag's
		uint32_t blockFirst = clip.flagFirst / SHADER_TYPES_GI_FLAG_WORD_COUNT;
		clip.cacheTableFirst = 2 * (myGiProbeHalf * myGiCacheBlockCount + blockFirst);
		clip.cacheTablePreviousFirst = 2 * ((1 - myGiProbeHalf) * myGiCacheBlockCount + blockFirst);
		clip.cacheRecordTableFirst = 4 * myGiCacheBlockCount + blockFirst;
		clip.cacheKeepFrameCount = myGiCacheKeepFrameCount;
		clip.cachePageCapacity = myGiCachePageCapacity;
		clip.cacheRecordCapacity = myGiCacheRecordCapacity;
		clip.cacheBlockCount = myGiCacheBlockCount;
		clip.cacheInit = myGiCacheInit ? 1U : 0U;
		clip.cacheGrowFirst = myGiCacheGrowFirst;
		clip.cacheSigma = myGiCacheSigma;
		clip.cacheEverywhere = myGiCacheEverywhere ? 1U : 0U;
		clip.depthFirst = clip.probeFirst * kDepthTexelCount;
		clip.depthPreviousFirst = clip.probePreviousFirst * kDepthTexelCount;
		clip.rayFirst = clipFirst * myGiRayCount;
		clip.rayCount = myGiRayCountThisFrame;
		clip.hysteresis = hysteresis;
		clip.reset = myGiReset ? 1 : 0;
		// the lights alone changed: the part of the static voxels' light they don't change is kept (see GiData::keptFirst)
		clip.lightsOnly = myGiLightsChanged && !myGiEmittersToggled && !myGiReset && !myGiClipJobs[clipIt].voxelize ? 1 : 0;
		// not for the first frames after a reset: the probes' depth maps are provisional then (see ComputeGiProbeUpdate),
		// and what leaked through them into the voxels fed the probes, which fed the voxels, for seconds (Sponza's arcade
		// started 8 times too bright and darkened over 200 frames)
		clip.bounce = myGiBounce && myGiFramesSinceReset >= kGiBounceDelayFrameCount ? 1 : 0;
		clip.adapt = myGiAdapt ? 1 : 0;
		clip.moving = myGiMovingJobCount > 0 ? 1 : 0;
		clip.dumpProbe = ~0U;
		clip.emissionReach = myGiEmissionReach;
		clip.referenceRayCount = myGiReferenceRayCount;
		// (a model all of which moves has a moving BVH alone: its static one is the default, a node that holds nothing)
		clip.trace = frame.traceTriangles && frame.model != nullptr && (frame.model->GetBvhNodeBuffer() != nullptr || frame.model->HasMovingBvh())
						 ? GI_TRACE_BVH
						 : GI_TRACE_VOXELS;
		clip.bvhMoving = frame.model != nullptr && frame.model->HasMovingBvh() ? 1 : 0;
		clip.nearReach = myGiNearOn ? myGiNearReach : 0.0F;
		clip.nearContact = std::clamp(frame.giNearContact, 0.0F, myGiNearReach);
		clip.nearPower = myGiNearPower;
		clip.nearShow = myGiNearShow;
		clip.nearTemporal = myGiNearTemporalOn ? 1U : 0U;
		clip.nearBlend = myGiNearFrameBlend;
		clip.nearFrame = myGiNearFrame;
		clip.nearStride = myGiNearStride;
		if (myGiDump)
		{
			// the probe nearest the dump point: by its lattice (the offsets are small)
			glm::vec3 origin(clip.probeOrigin[0], clip.probeOrigin[1], clip.probeOrigin[2]);
			glm::ivec3 nearest = glm::clamp(
				glm::ivec3(glm::floor((myGiDumpPoint - origin) / clip.probeOrigin[3])),
				glm::ivec3(0),
				glm::ivec3(clip.probeSize[0], clip.probeSize[1], clip.probeSize[2]) - 1);
			clip.dumpProbe = (static_cast<uint32_t>(nearest.z) * clip.probeSize[1] + static_cast<uint32_t>(nearest.y)) * clip.probeSize[0] + static_cast<uint32_t>(nearest.x);
			for (int axisIt = 0; axisIt < 3; axisIt++)
				clip.dumpPoint[axisIt] = myGiDumpPoint[axisIt];
			if (myGiDumpPixel)
			{
				clip.dumpPixel[0] = static_cast<float>(myGiDumpPixel->x);
				clip.dumpPixel[1] = static_cast<float>(myGiDumpPixel->y);
				clip.dumpPixel[3] = 1.0F;
			}
		}
	}
	myGiReset = false;
	auto clipData = std::span(data).first(myGiClipmap.clips.size());
	auto cells = gi::BinLights(clipData, frame.lights);
	write(InternalReserve(myGiLightCells, pipeline, frame.frameIndex, cells.ranges.size() / 2), std::as_bytes(std::span(cells.ranges)));
	write(InternalReserve(myGiLightIndices, pipeline, frame.frameIndex, std::max<size_t>(cells.indices.size(), 1)), std::as_bytes(std::span(cells.indices)));
	// the debug view, which its references (6 and 7) trace for (see GiData::debug)
	uint32_t debugView = frame.pushConstants.debugView;
	uint32_t debug = debugView >= DEBUG_VIEW_GI_FIRST && debugView < DEBUG_VIEW_GI_END ? debugView - DEBUG_VIEW_GI_FIRST + 1 : 0;
	{
		auto& statistics = myFrameStatisticsThisFrame;
		statistics.indirectLighting = true;
		statistics.giClipCount = static_cast<uint32_t>(clipData.size());
		statistics.giProbeCount = probeTotal;
		statistics.giActiveProbeCount = myGiActiveProbeCount;
		statistics.giRayCount = myGiRayCountThisFrame;
		statistics.giHysteresis = myGiHysteresis;
		statistics.giTraceTriangles = !clipData.empty() && clipData.front().trace == GI_TRACE_BVH;
		for (size_t clipIt = 0; clipIt < clipData.size(); clipIt++)
		{
			statistics.giVoxelSize[clipIt] = clipData[clipIt].volumeOrigin[3];
			statistics.giVoxelCounts[clipIt] = {clipData[clipIt].volumeSize[0], clipData[clipIt].volumeSize[1], clipData[clipIt].volumeSize[2]};
			statistics.giClipsVoxelized += myGiClipJobs[clipIt].voxelize ? 1 : 0;
		}
		statistics.giMovingDraws = myGiMovingJobCount;
		statistics.giMovingTriangles = myGiMovingTriangleCount;
	}
	for (auto& clip : clipData)
	{
		clip.enabled = 1;
		clip.lightCount = static_cast<uint32_t>(frame.lights.size());
		clip.debug = debug;
	}
	write(myGiDataBuffers[frame.frameIndex], std::as_bytes(std::span(data)));
}

void Renderer::InternalDrawViews(FrameGraph::PassContext& context, Pipeline& pipeline, const FrameInputs& inputs, ListId listId, bool late, bool prepass)
{
	const auto& list = prepass ? myPrepassList : InternalList(listId);
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
	// the variant most of its draws bind (the main list's opaque ones shade over the pre-pass's depth)
	pipeline.BindPipelineAuto(
		cmd,
		{.blend = kOpaqueBlend,
		 .fragmentShader = prepass ? kPrepassFragmentShader : uint8_t{0},
		 .depthTest = !prepass && listId == ListId::kMain ? DepthTest::kEqual : DepthTest::kLess});
	// the main list's fragments take the traced shadows and the indirect lighting's near field (at the pre-pass's depth:
	// its blended ones don't, see FragmentTransparent), the transmissive phase's don't
	auto pushConstants = inputs.pushConstants;
	pushConstants.prepassImages = 0;
	if (!prepass && listId == ListId::kMain)
		pushConstants.prepassImages = (myShadowsTraced ? PREPASS_IMAGE_SHADOW_MASK : 0U) | (myGiNearOn ? PREPASS_IMAGE_GI_NEAR : 0U) |
									   (myEmittersOn ? PREPASS_IMAGE_EMITTERS : 0U);
	// the pre-pass skips the blended draws (the main list's, left as they are in myPrepassList)
	auto prepassed = [](const DrawItem& item) { return item.variant.fragmentShader == kPrepassFragmentShader; };
	std::optional<core::std_extra::function_ref<bool(const DrawItem&)>> include;
	if (prepass)
		include = prepassed;

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
			RecordCulledDrawList(cmd, pipeline, list, pushConstants, viewIt, *culled, inputs.executor, include);
			continue;
		}
		std::optional<Frustum> frustum;
		if (inputs.culling && viewIt < inputs.viewProjections.size())
			frustum = Frustum::FromViewProjection(inputs.viewProjections[viewIt]);
		RecordDrawList(cmd, pipeline, list, pushConstants, viewIt, *myDrawStream, frustum ? &*frustum : nullptr, inputs.executor, include);
	}

	std::vector<CommandBuffer> secondaries;
	secondaries.push_back(std::move(cmd));
	CommandEncoder(context.cmd).ExecuteCommands(std::move(secondaries));
}

} // namespace gfx
