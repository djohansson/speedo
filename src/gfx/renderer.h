#pragma once

#include <gfx/camera.h>
#include <gfx/clusters.h>
#include <gfx/drawlist.h>
#include <gfx/framegraph.h>
#include <gfx/gpu.h>
#include <gfx/indirectlight.h>
#include <gfx/lightgizmos.h>
#include <gfx/renderoutput.h>
#include <gfx/shaders/capi.h>
#include <gfx/shadows.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <core/concurrentaccess.h>
#include <core/std_extra.h>

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <memory>
#include <span>
#include <vector>

namespace core
{
class TaskExecutor;
} // namespace core

namespace gfx
{

class Model;

// for FrameInputs' callables (called during Record, see std_extra::function_ref): no material is
[[nodiscard]] inline bool NoMaterial(size_t /*material*/) noexcept { return false; }

// what a frame draws, and with what (see Renderer::Record). its callables are called during Record
struct FrameInputs
{
	// the frame's slot of the per frame buffers (see SHADER_TYPES_FRAME_COUNT), and the output's image it is drawn into
	// (see RenderOutput)
	uint16_t frameIndex = 0;
	uint16_t outputIndex = 0;
	PushConstants pushConstants{}; // the frame's (Record fills in what the render target decides)
	// the scene: the model (or none), and which of its materials are transmissive (drawn in the main pass's second phase)
	const Model* model = nullptr;
	core::std_extra::function_ref<bool(size_t material)> transmissive = NoMaterial;
	// which scatter light under their surface (KHR_materials_volume_scatter: drawn first into the scatter pass's target too)
	core::std_extra::function_ref<bool(size_t material)> scatters = NoMaterial;
	// and which layer groups each material's pipeline can skip (see BuildDrawList): none, the generic ones, if not given
	std::optional<core::std_extra::function_ref<uint16_t(size_t material)>> specialization;
	// the views: their grid, and each one's viewport (its cell, letterboxed to its camera's aspect ratio)
	glm::uvec2 grid{1, 1};
	std::vector<ViewportCreateDesc> viewports;
	// each view's view projection, which its draws are culled by (and the shadow views by theirs), if culling
	std::vector<glm::mat4> viewProjections;
	bool culling = true;
	// whether the lights are binned into clusters per view, each fragment looping over its cluster's (else over all)
	bool clusteredLights = true;
	// whether the indirect lighting runs (see gfx/indirectlight.h): turned off, its volumes are dropped, and turned on
	// again they are voxelized again
	bool indirectLighting = false;
	// whether its rays trace the model's BVHs (GI_TRACE_BVH, where it has them) or walk the voxels (GI_TRACE_VOXELS)
	bool traceTriangles = true;
	// its voxels along the scene's longest axis (see gi::MakeClipmap): a change makes the clipmap again
	uint32_t giResolution = gi::kDefaultResolution;
	// whether the surface cache refines the probes' light (see GiData::cacheRayCount)
	bool giSurfaceCache = true;
	// whether the main pass's opaque fragments take the near field's rays near other surfaces (see GiData::nearReach),
	// where the indirect lighting's rays trace the BVHs, and how near what they meet is a contact (GiData::nearContact)
	bool giNearField = true;
	float giNearContact = gi::kDefaultNearContact;
	// and whether the near field is accumulated over frames (see GiData::nearTemporal)
	bool giNearTemporal = true;
	// the lights (gLights, as the shader sees them), and whether they cast shadows: fitted to the camera (the first view)
	// and the scene's bounds
	std::span<const LightData> lights;
	size_t modelLightCount = 0; // the first of lights are the model's, the rest the environment's (see LightCapture)
	bool captureLights = false; // keep a copy of them and their shadow views (see Renderer::GetLightCapture)
	bool shadows = true;
	// whether the brightest lights' shadows are traced through the model's BVHs (see ShadowTraceData), where it has them;
	// and at every pixel, rather than where the shadow atlas isn't sure (ShadowTraceData::mapped)
	bool tracedShadows = true;
	bool tracedShadowsEverywhere = false;
	// whether the model's emissive surfaces light the scene (see EmitterData), where it has BVHs for their shadow rays
	bool emissiveLights = true;
	shadows::ShadowCamera camera;
	Bounds3f sceneBounds;
	// how far what the camera looks at is (the median distance of the last frame's drawn pixels from the eye, smoothed):
	// the indirect lighting's finer clips are centered that far ahead of the eye, at most a share of their size (see
	// gi::MakeClipmap)
	float lookDistance = 0.0F;
	// the frame's histograms (SHADER_TYPES_HISTOGRAM_BINS per frame index: the exposure's bins, then the distances'),
	// host visible: ComputeMain counts into the frame's, which the host reads once the frame is done
	Buffer* exposureHistogram = nullptr;
	// the queue the frame is recorded for (its pool's secondary command buffers record the views), and its timeline
	Queue* graphicsQueue = nullptr;
	uint64_t graphicsTimeline = 0;
	// the ui: records its uploads (outside of a render pass), and its draws (in the swapchain's)
	core::std_extra::function_ref<void(CommandBufferHandle cmd)> prepareUi = NoUi;
	core::std_extra::function_ref<void(CommandBufferHandle cmd)> drawUi = NoUi;
	// the ui's frosted panels (see Scene::FrameTarget::uiBlurRects)
	std::span<const UiBlurRect> uiBlurRects;
	// runs tasks in the background (creating pipelines, see RecordDrawList), if given
	core::TaskExecutor* executor = nullptr;
};

// what the renderer planned for a frame, on the cpu (see Renderer::GetFrameStatistics), for the Statistics window
struct FrameStatistics
{
	// the draw lists, in order (main, the main pass's transmissive phase, shadow, scatter, scatter back faces): per list
	// its draws (chunks of submeshes, see mesh::Chunk) and their triangles (times their instances), for one view
	static constexpr std::array<const char*, 5> kListNames{"Main", "Main Transmissive", "Shadow", "Scatter", "Scatter Back"};
	std::array<uint32_t, kListNames.size()> listDraws{};
	std::array<uint64_t, kListNames.size()> listTriangles{};
	// what the gpu's culling tested per group (CULL_STATISTIC_MAIN and _SHADOW): each list it culls by the views it is
	// drawn in (the counts the gpu's decisions, Renderer::GetCullStatistics, are out of)
	std::array<uint64_t, CULL_STATISTIC_GROUP_COUNT> testedDraws{};
	std::array<uint64_t, CULL_STATISTIC_GROUP_COUNT> testedTriangles{};
	bool culling = false; // whether the views cull at all (View > Culling)
	bool gpuCulling = false; // else the cpu culls by the frustum, and the gpu's statistics stay as they were
	bool occlusion = false; // the main views' occlusion culling ran this frame (see Renderer::GetOcclusionModeName)
	bool shadowOcclusion = false;
	uint32_t mainViewCount = 0;
	// the lights: all of them, those with shadows and the shadow views they take (see shadows::Plan)
	uint32_t lightCount = 0;
	uint32_t shadowedLightCount = 0;
	uint32_t shadowViewCount = 0;
	// the clusters (see clusters::Plan): whether they are planned, the light indices of all views, and per cluster
	// reached by any light, the most lights and their mean
	bool clusteredLights = false;
	uint32_t clusterLightIndexCount = 0;
	uint32_t clusterMaxLightCount = 0;
	float clusterMeanLightCount = 0.0F;
	// the indirect lighting (see gfx/indirectlight.h): whether it ran, per clip its voxel size (world units) and voxels
	// per axis, how many clips were voxelized this frame (moved, or the scene changed), what moves (voxelized every
	// frame over the rest), the probes of all clips and the rays each traces a frame, and the hysteresis they blend by
	bool indirectLighting = false;
	bool giTraceTriangles = false; // whether the rays traced the BVHs this frame (see FrameInputs::traceTriangles)
	uint32_t giClipCount = 0;
	std::array<float, SHADER_TYPES_GI_CLIP_COUNT> giVoxelSize{};
	std::array<glm::uvec3, SHADER_TYPES_GI_CLIP_COUNT> giVoxelCounts{};
	uint32_t giClipsVoxelized = 0;
	uint32_t giMovingDraws = 0;
	uint32_t giMovingTriangles = 0;
	uint32_t giProbeCount = 0;
	uint32_t giActiveProbeCount = 0; // traced last frame (the lattice's near surfaces)
	uint32_t giRayCount = 0; // per probe this frame, by the budget
	float giHysteresis = 0.0F;
};

// draws frames: the lights' shadows (see gfx/shadows.h), the light under scattering materials' surfaces, the main pass (in two phases with transmissive materials), the order independent transparency's
// resolve, tonemapping and the backdrop in ComputeMain, and the ui, as a FrameGraph whose transients are the main render
// target, the transparency's lists and the transmission texture.
class Renderer final
{
public:
	// draws into output (see RenderOutput), which must outlive the renderer
	Renderer(Device& device, RenderOutput& output);
	~Renderer();

	// (re)creates the transients for the swapchain's extent (the gpu must be done with the previous ones) and points the
	// descriptors at them: the render target's color (in every frame's slot), the transmission texture and the
	// transparency's lists. nothing if the extent is the same.
	void Resize(Device& device, Pipeline& pipeline, rhi::Extent2d extent);

	// puts the transients that are sampled without being written every frame in a layout their descriptors allow (call
	// after Resize, before the next frame)
	void Prepare(CommandBufferHandle cmd);

	// records a frame into cmd
	void Record(CommandBufferHandle cmd, Pipeline& pipeline, const FrameInputs& inputs);

	[[nodiscard]] const FrameGraph& GetGraph() const noexcept { return myGraph; }

	// how long each pass takes on the gpu (milliseconds, by name): a moving average over the frames that timed it, for
	// the passes timed in the last 2 seconds, in the order they were first timed. any thread.
	[[nodiscard]] std::vector<std::pair<std::string, double>> GetPassTimings() const;

	// the lights and shadow views of the last frame recorded with FrameInputs::captureLights, for debugging. any thread.
	[[nodiscard]] LightCapture GetLightCapture() const;

	// what the occlusion culling decided in the last frame the gpu finished (CULL_STATISTIC_*), and the occlusion mode's
	// name. any thread.
	[[nodiscard]] std::array<uint32_t, CULL_STATISTIC_COUNT * CULL_STATISTIC_GROUP_COUNT> GetCullStatistics() const;
	[[nodiscard]] std::string_view GetOcclusionModeName() const noexcept;
	// what the cpu planned for the last frame recorded. any thread.
	[[nodiscard]] FrameStatistics GetFrameStatistics() const;

private:
	// the frame's draw lists, in the order their records and culling candidates are written
	enum class ListId : uint8_t
	{
		kMain,
		kMainTransmissive,
		kShadow,
		kScatter,
		kScatterBack,
		kCount,
	};
	[[nodiscard]] DrawList& InternalList(ListId list) noexcept;
	[[nodiscard]] const DrawList& InternalList(ListId list) const noexcept { return const_cast<Renderer*>(this)->InternalList(list); }

	// per frame index buffers bound as one array (binding), which grow to the next power of two when a frame needs more
	// than they hold: the replaced one is kept until no frame in flight can use it (myRetiredBuffers)
	struct GrowingBuffers
	{
		std::string name;
		std::string binding;
		size_t elementSize = 0;
		rhi::BufferUsage usage = rhi::BufferUsage::kStorage;
		rhi::MemoryProperty memory = rhi::MemoryProperty::kHostVisible;
		std::vector<Buffer> buffers;
		std::vector<size_t> capacities;
	};
	void InternalCreate(GrowingBuffers& buffers, Device& device, size_t capacity);
	Buffer& InternalReserve(GrowingBuffers& buffers, Pipeline& pipeline, uint16_t frameIndex, size_t count);
	// plans the gpu's culling of the frame's draw lists (see CullCandidate), and writes it
	void InternalPlanCulling(Pipeline& pipeline, const FrameInputs& frame);
	// where the gpu's culling put a list's draws for a view (of the views of its pass: the main views, or the shadow
	// views), if it culled them: the first draws (or all), or the late ones (see CULL_JOB_LATE)
	[[nodiscard]] std::optional<CulledDraws> InternalCulledDraws(ListId list, uint32_t view, bool late = false) const;
	// whether the gpu culls a list this frame (else the cpu does, by the frustum)
	[[nodiscard]] bool InternalCullsOnGpu(ListId list, const FrameInputs& frame) const noexcept;

	// plans the frame's indirect lighting (see gfx/indirectlight.h): the volume for the model (voxelized again when it
	// changes), the lights per cell, and the passes
	void InternalPlanGi(Pipeline& pipeline, const FrameInputs& frame);
	// a persistent buffer of count elements of size bound as binding (all of the frame indices' slots if perFrame), the
	// one it replaces kept until no frame in flight can use it
	void InternalReplaceBuffer(
		Buffer& buffer, Pipeline& pipeline, std::string_view name, std::string_view binding, size_t size,
		rhi::BufferUsage usage = rhi::BufferUsage::kStorage | rhi::BufferUsage::kTransferDestination); // the GI Clear pass fills them

	// records a list's draws for the views (each in its viewport); prepass: the main list's opaque draws as the pre-pass's
	// (myPrepassList, the same runs and culled commands)
	void InternalDrawViews(
		FrameGraph::PassContext& context, Pipeline& pipeline, const FrameInputs& inputs, ListId list, bool late = false, bool prepass = false);

	FrameGraph myGraph;
	FrameGraph::ImageId myColor;
	FrameGraph::ImageId myDepth;
	// the pre-pass's geometry normals (SHADER_TYPES_PREPASS_NORMAL_TEXTURE, see PrepassOutput), with the depth, which the
	// main pass then shades over (DepthTest::kEqual) and the traced shadows start from
	FrameGraph::ImageId myPrepassNormal;
	// the traced shadows' (see SHADER_TYPES_SHADOW_RAW_TEXTURE): the rays' visibility and blockers' distances, the
	// visibility filtered along rows, and along columns (what the main pass takes); each a channel per traced light
	FrameGraph::ImageId myShadowRaw;
	FrameGraph::ImageId myShadowBlocker;
	FrameGraph::ImageId myShadowRows;
	FrameGraph::ImageId myShadowMask;
	FrameGraph::BufferId myShadowTraceList;
	FrameGraph::BufferId myShadowTraceTiles;
	// the indirect lighting's near field (see SHADER_TYPES_GI_NEAR_RAW_TEXTURE): the rays' radiance and whether they met no
	// contact, that filtered along rows, and along columns (what the main pass takes)
	FrameGraph::ImageId myGiNearRaw;
	FrameGraph::ImageId myGiNearRows;
	FrameGraph::ImageId myGiNear;
	FrameGraph::ImageId myTransmission;
	FrameGraph::ImageId myScatter;
	FrameGraph::ImageId myScatterDepth;
	FrameGraph::ImageId myScatterAlbedo;
	FrameGraph::ImageId myScatterBackDepth;
	FrameGraph::ImageId myScatterNoisy;
	FrameGraph::ImageId myScatterLight;
	// the ui's frosted panels' backdrop: the output under them and within the blur's reach (linear), and that blurred
	// along x (see SHADER_TYPES_UI_BLUR_SOURCE_TEXTURE)
	FrameGraph::ImageId myUiBlurSource;
	FrameGraph::ImageId myUiBlurRows;
	RenderOutput& myRenderOutput;
	FrameGraph::ImageId myOutput;
	FrameGraph::ImageId myShadowAtlas;
	FrameGraph::BufferId myOitHeads;
	FrameGraph::BufferId myOitNodes;
	FrameGraph::BufferId myOitCounter;
	FrameGraph::BufferId myExposureHistogram;
	FrameGraph::PassId myTransmissionPass;
	FrameGraph::PassId myTransmissivePhase;
	FrameGraph::PassId myShadowPass;
	FrameGraph::PassId myScatterBackPass;
	FrameGraph::PassId myScatterPass;
	FrameGraph::PassId myScatterBlurPass;
	FrameGraph::PassId myScatterDenoisePass;
	FrameGraph::PassId myUiBlurXPass;
	FrameGraph::PassId myUiBlurYPass;

	// per frame index: the shadows' views and each light's first (gShadowViews, gLightShadows), host visible
	std::vector<Buffer> myShadowViewBuffers;
	std::vector<Buffer> myLightShadowBuffers;
	// the frame's traced shadows (gShadowTrace[frame], see ShadowTraceData), and whether any light's are
	std::vector<Buffer> myShadowTraceBuffers;
	bool myShadowsTraced = false;
	FrameGraph::PassId myShadowTraceClearPass;
	FrameGraph::PassId myShadowClassifyPass;
	FrameGraph::PassId myShadowTracePass;
	FrameGraph::PassId myShadowDenoiseXPass;
	FrameGraph::PassId myShadowDenoiseYPass;
	shadows::ShadowPlan myShadowPlan;
	// per frame index: the clusters' views, ranges and light indices (gClusterViews, gClusters, gClusterLights), host
	// visible
	std::vector<Buffer> myClusterViewBuffers;
	std::vector<Buffer> myClusterBuffers;
	std::vector<Buffer> myClusterLightBuffers;
	// per frame index: the ui's frosted panels (gUiBlurRects), host visible, and the UI Blur passes' thread counts
	std::vector<Buffer> myUiBlurRectBuffers;
	// per frame index, the draw lists' DrawRecords (gDrawRecords)
	GrowingBuffers myDrawRecords;
	// the gpu's culling (the Cull pass, see CullCandidate): per frame index its candidates (growing), jobs and views
	// (host visible), and the commands and counts it writes (growing, device local), bound for the passes drawing them
	// (myCullCommands, myCullCounts). each list's first job (kNoJob: not culled on the gpu), and the job count, and the
	// most candidates of a job, the Cull pass's dispatch
	static constexpr uint32_t kNoJob = ~0U;
	bool myGpuCulling = false;
	GrowingBuffers myCullCandidates;
	GrowingBuffers myCullCommandBuffers;
	GrowingBuffers myCullCountBuffers;
	std::vector<Buffer> myCullJobBuffers;
	std::vector<Buffer> myCullViewBuffers;
	FrameGraph::BufferId myCullCommands;
	FrameGraph::BufferId myCullCounts;
	FrameGraph::PassId myCullClearPass;
	FrameGraph::PassId myCullPass;
	std::array<uint32_t, std::to_underlying(ListId::kCount)> myCullFirstJob{};
	std::array<uint32_t, std::to_underlying(ListId::kCount)> myCullLateFirstJob{};
	// the Cull pass's (myCullEarlyJobCount), then the Cull Shadows Late pass's, then the Cull Late pass's
	std::vector<CullJob> myCullJobs;
	uint32_t myCullEarlyJobCount = 0;
	uint32_t myCullShadowLateJobBase = 0;
	uint32_t myCullShadowLateJobCount = 0;
	uint32_t myCullShadowLateMaxCandidateCount = 0;
	uint32_t myCullMainLateJobBase = 0;
	uint32_t myCullCountCount = 0;
	uint32_t myCullMaxCandidateCount = 0;
	uint32_t myCullLateMaxCandidateCount = 0;
	// occlusion culling (see the Depth Pyramid pass): whether it is wanted (SPEEDO_OCCLUSION_CULLING), and on this frame;
	// the main pass's draws seen last frame first, then a depth pyramid of the main depth, then the rest that it doesn't
	// hide (Main Late). the visibility per draw and main view (gCullVisibility), kept from frame to frame, growing
	bool myOcclusionCulling = true;
	bool myShadowOcclusionCulling = true;
	bool myOcclusionThisFrame = false;
	// how what is hidden is found (SPEEDO_OCCLUSION_MODE): two-phase (the draws seen last frame, a pyramid of them, then
	// the rest), reprojected (one phase, against the last frame's depth reprojected, see ComputeDepthReproject) or hybrid
	// (what that doesn't hide first, then the rest against a pyramid of it)
	enum class OcclusionMode : uint8_t
	{
		kTwoPhase,
		kReprojected,
		kHybrid,
	};
	OcclusionMode myOcclusionMode = OcclusionMode::kTwoPhase;
	// the depth reprojection's: the last frame's depth of what doesn't move (gPreviousDepth, kept: recreated, and not
	// valid, on resizes), its extent and views, and the frame's reprojected (gReprojectedDepth, a transient), and the
	// main depth's stencil, which marks what moves
	FrameGraph::BufferId myPreviousDepth;
	FrameGraph::BufferId myReprojectedDepth;
	Buffer myPreviousDepthBuffer;
	rhi::Extent2d myPreviousExtent{};
	bool myPreviousValid = false;
	std::vector<glm::mat4> myPreviousInverseViewProjections;
	std::vector<rhi::Viewport> myPreviousViewports;
	std::unique_ptr<ImageView> myStencilView;
	FrameGraph::PassId myDepthReprojectClearPass;
	FrameGraph::PassId myDepthReprojectPass;
	FrameGraph::PassId myDepthPyramidReprojectedPass;
	FrameGraph::PassId myCullReprojectedPass;
	FrameGraph::PassId myDepthSavePass;
	// per frame index, what the occlusion culling decided (gCullStatistics), host visible: read after the frame's fence,
	// the last frame's kept, and with SPEEDO_GPU_TIMINGS=1 summed for the destructor's averages
	std::vector<Buffer> myCullStatisticsBuffers;
	FrameGraph::BufferId myCullStatisticsId;
	core::ConcurrentAccess<std::array<uint32_t, CULL_STATISTIC_COUNT * CULL_STATISTIC_GROUP_COUNT>> myCullStatistics;
	std::array<uint64_t, CULL_STATISTIC_COUNT * CULL_STATISTIC_GROUP_COUNT> myCullStatisticSums{};
	uint64_t myCullStatisticFrameCount = 0;
	FrameStatistics myFrameStatisticsThisFrame; // filled in while recording, then published
	core::ConcurrentAccess<FrameStatistics> myFrameStatistics;
	bool myCountEmulated = false;
	FrameGraph::ImageId myDepthPyramid;
	std::vector<std::unique_ptr<ImageView>> myDepthPyramidViews; // a storage view per level
	FrameGraph::BufferId myCullVisibility;
	Buffer myCullVisibilityBuffer;
	size_t myCullVisibilityCapacity = 0;
	FrameGraph::PassId myDepthPyramidPass;
	FrameGraph::PassId myCullLatePass;
	FrameGraph::PassId myPrepassLatePass;
	FrameGraph::PassId myMainLatePass;
	// the shadow views' occlusion culling, as the main pass's two-phase: the casters each saw last frame first (Shadows),
	// a pyramid of the atlas (Shadow Depth Pyramid), then the rest it doesn't hide (Cull Shadows Late, Shadows Late)
	bool myShadowOcclusionThisFrame = false;
	FrameGraph::ImageId myShadowDepthPyramid;
	std::vector<std::unique_ptr<ImageView>> myShadowDepthPyramidViews;
	FrameGraph::PassId myShadowDepthPyramidPass;
	FrameGraph::PassId myCullShadowsLatePass;
	FrameGraph::PassId myShadowsLatePass;

	// the indirect lighting (FrameInputs::indirectLighting; SPEEDO_GI_RESOLUTION voxels along the scene's longest axis, SPEEDO_GI_CLIP_COUNT
	// clips): its clipmap, what it was made for (the model, its bounds and its draws), the persistent buffers (voxels,
	// radiance, rays, the probes' irradiance and depth: see GiData), per frame index its parameters (host visible), and
	// what the voxelizer and the voxels' lighting read (growing). per clip, whether it is voxelized this frame (it moved,
	// or the scene changed) and its static jobs then (the first in gGiJobs, their count and triangles). the probes' rays
	// a frame (SPEEDO_GI_RAY_COUNT) and the frames their set of directions takes (SPEEDO_GI_RAY_PHASE_COUNT, and which
	// subset is the frame's: GiData::rayPhase), their spacing (SPEEDO_GI_PROBE_SPACING), what they keep of a frame
	// (SPEEDO_GI_HYSTERESIS), whether the voxels take their light (SPEEDO_GI_BOUNCE), which half of the probes' buffers
	// is the frame's (see GiData::probeFirst), and whether the frame keeps nothing of the last (GiData::reset)
	uint32_t myGiResolution = gi::kDefaultResolution; // the clipmap's (FrameInputs::giResolution, when it was made)
	uint32_t myGiClipCount = 3;
	float myGiLookAhead = 0.5F; // SPEEDO_GI_LOOK_AHEAD: how far ahead of the eye the finer clips are centered, see gi::MakeClipmap
	float myGiEmissionReach = 0.5F; // SPEEDO_GI_EMISSION_REACH: see GiData::emissionReach
	uint32_t myGiRayCount = 128; // at most: SPEEDO_GI_RAY_COUNT
	uint32_t myGiRayBudget = 500000; // rays a frame over all probes (SPEEDO_GI_RAY_BUDGET): fewer per probe where more are traced
	uint32_t myGiReferenceRayCount = 256; // SPEEDO_GI_REFERENCE_RAYS, see GiData::referenceRayCount
	uint32_t myGiRayCountThisFrame = 128;
	uint32_t myGiActiveProbeCount = 0; // what the last frame of this index counted (see gGiStatistics)
	uint64_t myGiActiveProbeSum = 0; // over the frames, for SPEEDO_GPU_TIMINGS' summary
	uint64_t myGiRayCountSum = 0;
	uint32_t myGiStatisticFrameCount = 0;
	std::vector<Buffer> myGiStatisticsBuffers;
	FrameGraph::BufferId myGiStatisticsId;
	uint32_t myGiRayPhaseCount = 32;
	uint32_t myGiRayPhase = 0;
	uint32_t myGiProbeSpacing = 8;
	float myGiHysteresis = 0.97F;
	bool myGiBounce = true;
	bool myGiAdapt = false;
	bool myGiDump = false; // SPEEDO_GI_DUMP: a probe per clip dumped at exit
	glm::vec3 myGiDumpPoint{0.0F};
	std::optional<glm::ivec2> myGiDumpPixel; // SPEEDO_GI_DUMP=pixel:<x>,<y>: the point is what is under the pixel
	std::array<float, SHADER_TYPES_GI_CLIP_COUNT * GI_DUMP_FLOAT_COUNT> myGiDumpValues{};
	uint32_t myGiProbeHalf = 0;
	bool myGiReset = true;
	// whether the indirect lighting's buffers are new this frame, and so cleared before anything reads them (see the GI
	// Clear pass)
	bool myGiClearBuffers = true;
	uint32_t myGiFrameStamp = 0; // see GiData::frameStamp
	uint32_t myGiFramesSinceReset = 0;
	bool myGiRevoxelize = false; // SPEEDO_GI_REVOXELIZE=1: every clip voxelized again every frame
	// SPEEDO_GI_EYE_PATH: where the clips are centered, a point a frame, and toward what (see gi::MakeClipmap)
	struct GiEyePoint
	{
		glm::vec3 eye{0.0F};
		std::optional<glm::vec3> look;
	};
	std::vector<GiEyePoint> myGiEyePath;
	size_t myGiEyePathNext = 0;
	bool myGiEyeLog = false; // SPEEDO_GI_EYE_LOG=1: each frame's
	gi::Clipmap myGiClipmap;
	const Model* myGiModel = nullptr;
	Bounds3f myGiBounds;
	size_t myGiItemCount = 0;
	struct GiClipJobs
	{
		bool voxelize = false;
		glm::ivec3 scroll{0}; // see GiData::scroll
		uint32_t base = 0;
		uint32_t count = 0;
		uint32_t triangleCount = 0;
	};
	std::array<GiClipJobs, SHADER_TYPES_GI_CLIP_COUNT> myGiClipJobs{};
	// what moves (skinned, morphed, animated), voxelized each frame over the static voxels (kept, see GI Keep): its jobs
	// after the static ones (from myGiMovingJobBase)
	uint32_t myGiMovingJobBase = 0;
	uint32_t myGiMovingJobCount = 0;
	uint32_t myGiMovingTriangleCount = 0;
	Buffer myGiStaticVoxelBuffer;
	FrameGraph::BufferId myGiStaticVoxels;
	FrameGraph::PassId myGiKeepPass;
	// the static voxels' direct light (GiData::directFirst) is made again when the lights change: the last frame's lights
	FrameGraph::PassId myGiLightStaticPass;
	std::vector<LightData> myGiLights;
	bool myGiLightsChanged = false;
	// and whether they changed a little (the same lights, turned and dimmed a little: the sky's sun as the time of day
	// advances), for how many frames more the surface cache follows them rather than starting over (see
	// kGiLightsFollowFrameCount)
	bool myGiLightsGradual = false;
	uint32_t myGiLightsFollowFrames = 0;
	// whether the emitters started or stopped lighting this frame (the static voxels' kept part of their light is then made
	// again, see GiData::lightsOnly)
	bool myGiEmittersToggled = false;
	Buffer myGiFlagBuffer;
	FrameGraph::BufferId myGiFlags;
	// per voxel which of its probes it sees, where the rays trace the BVHs (see ComputeGiResolve)
	Buffer myGiVisibilityBuffer;
	FrameGraph::BufferId myGiVisibility;
	// the surface cache (gGiCache, see GiData::cacheRayCount): FrameInputs::giSurfaceCache, SPEEDO_GI_CACHE_RAYS rays a voxel a
	// frame, SPEEDO_GI_CACHE_SAMPLES the most samples a voxel's mean is of, and its filter's passes and edge
	// (SPEEDO_GI_CACHE_FILTER_PASSES, SPEEDO_GI_CACHE_SIGMA: see GiData::cacheFilterPassCount)
	// its storage (see GiCachePopPage), only while it is on: the pages (gGiCache), the filter's records (gGiCacheRecords),
	// the blocks' page and record tables (gGiCacheBlocks) and the free pages (gGiCacheFree); the pages and records they
	// hold (0: none made), grown to what the resolve counted (GI_STATISTIC_CACHE_*, a frame or two behind), and the
	// blocks of the clipmap they were made for
	Buffer myGiCacheBuffer;
	FrameGraph::BufferId myGiCache;
	Buffer myGiCacheRecordBuffer;
	FrameGraph::BufferId myGiCacheRecords;
	Buffer myGiCacheTableBuffer;
	FrameGraph::BufferId myGiCacheTables;
	Buffer myGiCacheFreeBuffer;
	FrameGraph::BufferId myGiCacheFree;
	uint32_t myGiCachePageCapacity = 0;
	uint32_t myGiCacheRecordCapacity = 0;
	uint32_t myGiCacheBlockCount = 0;
	uint32_t myGiCacheBlocksCounted = 0; // the resolve's GI_STATISTIC_CACHE_BLOCKS
	uint32_t myGiCacheActiveBlocksCounted = 0; // and GI_STATISTIC_CACHE_ACTIVE_BLOCKS
	// this frame: whether the storage is made anew (GiData::cacheInit), the threads the prepare runs, and the pages and
	// free list the pool grew from (copied by the GI Cache Grow pass, then retired: GiData::cacheGrowFirst their count)
	bool myGiCacheInit = false;
	uint32_t myGiCachePrepareThreadCount = 0;
	uint32_t myGiCacheGrowFirst = 0;
	Buffer myGiCacheGrowPages;
	Buffer myGiCacheGrowFree;
	FrameGraph::PassId myGiCacheGrowPass;
	uint32_t myGiCacheRayCount = 1;
	uint32_t myGiCacheSampleCount = 1024;
	uint32_t myGiCacheFilterPassCount = 3;
	float myGiCacheSigma = 8.0F;
	bool myGiCacheEverywhere = false; // SPEEDO_GI_CACHE_EVERYWHERE=1: see GiData::cacheEverywhere
	uint32_t myGiCacheKeepFrameCount = 1024; // SPEEDO_GI_CACHE_KEEP_FRAMES: see GiData::cacheKeepFrameCount
	bool myGiCacheShow = false; // SPEEDO_GI_CACHE_SHOW=1: see GiData::cacheShow
	bool myGiSurfaceCacheOn = false; // this frame (FrameInputs::giSurfaceCache), for the GI pass's filter
	// the near field (see GiData::nearReach): its rays' reach in voxels (SPEEDO_GI_NEAR_REACH), the power of the share that
	// met no contact the volume's lookup is weighed by (SPEEDO_GI_NEAR_POWER), what the shading shows of it
	// (SPEEDO_GI_NEAR_SHOW), whether it runs this frame (FrameInputs::giNearField, where the rays trace the BVHs), and its
	// passes
	float myGiNearReach = 8.0F;
	float myGiNearPower = 2.0F;
	uint32_t myGiNearShow = 0;
	bool myGiNearOn = false;
	FrameGraph::PassId myGiNearPass;
	FrameGraph::PassId myGiNearXPass;
	FrameGraph::PassId myGiNearYPass;
	// the near field accumulated over frames (see ComputeGiNearTemporal): its history, two of each (by the frame's parity:
	// SHADER_TYPES_GI_NEAR_HISTORY_TEXTURE), made with the frame graph's transients; the frame's weight in it
	// (SPEEDO_GI_NEAR_HISTORY_FRAME_COUNT, its reciprocal), whether last frame's holds anything (it ran, at this size), and
	// the frames it has run (GiData::nearFrame)
	struct ScreenImage
	{
		std::unique_ptr<Image> image;
		std::unique_ptr<ImageView> view;
	};
	std::array<ScreenImage, 2> myGiNearHistoryImages;
	std::array<ScreenImage, 2> myGiNearHistoryDistanceImages;
	std::array<FrameGraph::ImageId, 2> myGiNearHistory{};
	std::array<FrameGraph::ImageId, 2> myGiNearHistoryDistance{};
	FrameGraph::PassId myGiNearTemporalPass;
	float myGiNearBlend = 1.0F / 16.0F;
	float myGiNearFrameBlend = 1.0F; // this frame's (GiData::nearBlend)
	bool myGiNearTemporalOn = false;
	bool myGiNearHistoryValid = false;
	uint32_t myGiNearFrame = 0;
	// emissive surfaces as lights (see EmitterData, emitters.slang): the frame's (gEmitterData[frame], host visible); the
	// reservoirs per traced pixel (gEmitterReservoirs, three parts, see ComputeEmitterShade), made at the frame's size;
	// the traced pixels' light, filtered along rows, and columns, and over frames with their surfaces (two of each, by the
	// frame's parity); the passes; the candidates a traced pixel resamples (SPEEDO_EMITTER_CANDIDATE_COUNT) and a voxel's
	// samples and their candidates (SPEEDO_EMITTER_VOXEL_SAMPLE_COUNT, SPEEDO_EMITTER_VOXEL_CANDIDATE_COUNT), the
	// reservoirs' reuse (SPEEDO_EMITTER_REUSE=0 without: over frames, capped at a count of SPEEDO_EMITTER_HISTORY_CAP times
	// the candidates, and of SPEEDO_EMITTER_SPATIAL_COUNT traced pixels within spatialRadius of them), the filter's reach
	// in pixels (SPEEDO_EMITTER_FILTER_REACH), the frame's weight over frames (SPEEDO_EMITTER_HISTORY_FRAME_COUNT, its
	// reciprocal), whether they light this frame, whether the last frame's history and reservoirs hold anything (they lit
	// it, at this size), and the frames they have lit
	std::vector<Buffer> myEmitterDataBuffers;
	Buffer myEmitterReservoirBuffer;
	FrameGraph::BufferId myEmitterReservoirs;
	FrameGraph::ImageId myEmitterRaw;
	FrameGraph::ImageId myEmitterRows;
	FrameGraph::ImageId myEmitterFiltered;
	std::array<ScreenImage, 2> myEmitterHistoryImages;
	std::array<ScreenImage, 2> myEmitterSurfaceImages;
	std::array<FrameGraph::ImageId, 2> myEmitterHistory{};
	std::array<FrameGraph::ImageId, 2> myEmitterSurfaces{};
	FrameGraph::PassId myEmitterSamplePass;
	FrameGraph::PassId myEmitterShadePass;
	FrameGraph::PassId myEmitterXPass;
	FrameGraph::PassId myEmitterYPass;
	FrameGraph::PassId myEmitterTemporalPass;
	uint32_t myEmitterCandidateCount = 8;
	uint32_t myEmitterVoxelSampleCount = 4;
	uint32_t myEmitterVoxelCandidateCount = 8;
	bool myEmitterReuse = false; // SPEEDO_EMITTER_REUSE=1, see ComputeEmitterSample
	float myEmitterHistoryCap = 20.0F;
	uint32_t myEmitterSpatialCount = 0;
	float myEmitterSpatialRadius = 8.0F;
	uint32_t myEmitterFilterReach = 16;
	// the traced pixels: every stride-th each way (SPEEDO_EMITTER_STRIDE), each a texel of the emitters' images; and how
	// many frames a point found visible stays so under a still camera (SPEEDO_EMITTER_VISIBILITY_FRAME_COUNT, see
	// EmitterData::visibilityFrameCount; a quarter of it where something moves)
	uint32_t myEmitterStride = 4;
	uint32_t myEmitterVisibilityFrameCount = 8;
	// the light below which a shadow ray is traced by chance (SPEEDO_EMITTER_ROULETTE, see EmitterData::roulette)
	float myEmitterRoulette = 0.02F;
	float myEmitterBlend = 1.0F / 32.0F; // 1 over SPEEDO_EMITTER_HISTORY_FRAME_COUNT
	bool myEmittersOn = false;
	bool myEmitterHistoryValid = false;
	uint32_t myEmitterFrame = 0;
	uint32_t myEmitterReferenceFrameCount = 0; // the frames the reference (DEBUG_VIEW_EMITTERS_REFERENCE) has accumulated
	// whether the static voxels' kept light (see ComputeGiLightStatic) has the emitters' in it: it is made again when
	// that changes
	bool myGiEmittersLit = false;
	FrameGraph::PassId myGiRestorePass;
	FrameGraph::PassId myGiVoxelizeMovingPass;
	Buffer myGiVoxelBuffer;
	Buffer myGiRadianceBuffer;
	Buffer myGiRayBuffer;
	Buffer myGiIrradianceBuffer;
	Buffer myGiProbeDepthBuffer;
	FrameGraph::BufferId myGiProbeDepth;
	// each voxel's resolved probes (gGiVolumes, see ComputeGiResolve): the clips stacked along z, made again with the
	// clipmap's sizes (the replaced ones kept as the buffers are)
	struct GiVolume
	{
		std::unique_ptr<Image> image;
		std::unique_ptr<ImageView> view;
	};
	std::array<GiVolume, SHADER_TYPES_GI_VOLUME_COUNT> myGiVolumeImages;
	std::array<FrameGraph::ImageId, SHADER_TYPES_GI_VOLUME_COUNT> myGiVolumes{};
	std::vector<std::pair<uint64_t, GiVolume>> myRetiredGiVolumes;
	void InternalReplaceGiVolumes(Pipeline& pipeline, const glm::uvec3& extent);
	std::vector<Buffer> myGiDataBuffers;
	GrowingBuffers myGiJobs;
	GrowingBuffers myGiLightCells;
	GrowingBuffers myGiLightIndices;
	FrameGraph::BufferId myGiVoxels;
	FrameGraph::BufferId myGiRadiance;
	FrameGraph::BufferId myGiRays;
	FrameGraph::BufferId myGiIrradiance;
	FrameGraph::PassId myGiClearPass;
	FrameGraph::PassId myGiShiftPass;
	FrameGraph::PassId myGiVoxelizePass;
	FrameGraph::PassId myGiPass;
	// the buffers replaced by bigger ones, with the Record that replaced them
	std::vector<std::pair<uint64_t, Buffer>> myRetiredBuffers;
	// per frame index, the indirect draws' commands (see RecordDrawList), and the one the frame recording takes from
	std::vector<DrawStream> myDrawStreams;
	DrawStream* myDrawStream = nullptr;
	uint32_t myUiBlurThreadsX = 0;
	uint32_t myUiBlurThreadsY = 0;
	DrawList myShadowDrawList;

	// what Record's passes (and the output's) read of the frame they record
	RecordingFrame myFrame;
	uint32_t myOitNodeCapacity = 0;

	// the passes' gpu timings, for the ui (see GetPassTimings), and with SPEEDO_GPU_TIMINGS=1 every frame's, whose medians
	// the destructor prints
	core::ConcurrentAccess<std::vector<std::pair<std::string, double>>> myPassTimings;
	struct SmoothedTiming
	{
		std::string name;
		double milliseconds = 0.0; // an exponential moving average of the frames that timed the pass
		uint64_t lastRecord = 0; // the Record that last timed it
	};
	std::vector<SmoothedTiming> mySmoothedTimings; // in the order the passes were first timed
	uint64_t myRecordCount = 0; // of the frames that timed passes (see GetPassTimings)
	uint64_t myFrameCount = 0; // Records, every one
	core::ConcurrentAccess<LightCapture> myLightCapture;
	bool myCollectTimings = false;
	std::vector<std::pair<std::string, std::vector<double>>> myCollectedTimings;

	// the frame's draw lists, per phase
	bool myTwoPhases = false;
	std::array<DrawList, 2> myDrawLists;
	// the main list's opaque draws as the pre-pass draws them (kPrepassFragmentShader; the blended ones left as they are,
	// for InternalDrawViews to skip): its items, runs and records, so the main list's culled commands draw it
	DrawList myPrepassList;
	DrawList myScatterBackDrawList;
	DrawList myScatterDrawList;
};

} // namespace gfx
