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
	uint16_t frameIndex = 0;
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
	// the lights (gLights, as the shader sees them), and whether they cast shadows: fitted to the camera (the first view)
	// and the scene's bounds
	std::span<const LightData> lights;
	size_t modelLightCount = 0; // the first of lights are the model's, the rest the environment's (see LightCapture)
	bool captureLights = false; // keep a copy of them and their shadow views (see Renderer::GetLightCapture)
	bool shadows = true;
	shadows::ShadowCamera camera;
	Bounds3f sceneBounds;
	// the exposure histograms (SHADER_TYPES_EXPOSURE_BINS per frame index), host visible: ComputeMain counts into the
	// frame's, which the host reads once the frame is done
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
	uint32_t giClipCount = 0;
	std::array<float, SHADER_TYPES_GI_CLIP_COUNT> giVoxelSize{};
	std::array<glm::uvec3, SHADER_TYPES_GI_CLIP_COUNT> giVoxelCounts{};
	uint32_t giClipsVoxelized = 0;
	uint32_t giMovingDraws = 0;
	uint32_t giMovingTriangles = 0;
	uint32_t giProbeCount = 0;
	uint32_t giRayCount = 0;
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
		Buffer& buffer, Pipeline& pipeline, std::string_view name, std::string_view binding, size_t size, rhi::BufferUsage usage = rhi::BufferUsage::kStorage);

	void InternalDrawViews(FrameGraph::PassContext& context, Pipeline& pipeline, const FrameInputs& inputs, ListId list, bool late = false);

	FrameGraph myGraph;
	FrameGraph::ImageId myColor;
	FrameGraph::ImageId myDepth;
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
	uint32_t myGiResolution = 128;
	uint32_t myGiClipCount = 3;
	uint32_t myGiRayCount = 128;
	uint32_t myGiRayPhaseCount = 32;
	uint32_t myGiRayPhase = 0;
	uint32_t myGiProbeSpacing = 8;
	float myGiHysteresis = 0.97F;
	bool myGiBounce = true;
	uint32_t myGiProbeHalf = 0;
	bool myGiReset = true;
	uint32_t myGiFrameStamp = 0; // see GiData::frameStamp
	bool myGiRevoxelize = false; // SPEEDO_GI_REVOXELIZE=1: every clip voxelized again every frame
	std::vector<glm::vec3> myGiEyePath; // SPEEDO_GI_EYE_PATH: where the clips are centered, a point a frame
	size_t myGiEyePathNext = 0;
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
	Buffer myGiFlagBuffer;
	FrameGraph::BufferId myGiFlags;
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
	DrawList myScatterBackDrawList;
	DrawList myScatterDrawList;
};

} // namespace gfx
