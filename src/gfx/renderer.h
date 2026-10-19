#pragma once

#include <gfx/camera.h>
#include <gfx/clusters.h>
#include <gfx/drawlist.h>
#include <gfx/framegraph.h>
#include <gfx/gpu.h>
#include <gfx/lightgizmos.h>
#include <gfx/renderoutput.h>
#include <gfx/shaders/capi.h>
#include <gfx/shadows.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <core/concurrentaccess.h>
#include <core/std_extra.h>

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

private:
	void InternalDrawViews(FrameGraph::PassContext& context, Pipeline& pipeline, const FrameInputs& inputs, const DrawList& list);

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
	std::vector<Buffer> myDrawRecordBuffers; // per frame index, the draw lists' DrawRecords (gDrawRecords)
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
	uint64_t myRecordCount = 0;
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
