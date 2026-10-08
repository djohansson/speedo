#pragma once

#include <gfx/camera.h>
#include <gfx/drawlist.h>
#include <gfx/framegraph.h>
#include <gfx/gpu.h>
#include <gfx/shaders/capi.h>
#include <gfx/shadows.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace gfx
{

class Model;

// what a frame draws, and with what (see Renderer::Record)
struct FrameInputs
{
	uint16_t frameIndex = 0;
	PushConstants pushConstants{}; // the frame's (Record fills in what the render target decides)
	// the scene: the model (or none), and which of its materials are transmissive (drawn in the main pass's second phase)
	const Model* model = nullptr;
	std::function<bool(size_t material)> transmissive;
	// the views: their grid, and each one's viewport (its cell, letterboxed to its camera's aspect ratio)
	glm::uvec2 grid{1, 1};
	std::vector<ViewportCreateDesc> viewports;
	// the lights (gLights, as the shader sees them), and whether they cast shadows: fitted to the camera (the first view)
	// and the scene's bounds
	std::span<const LightData> lights;
	bool shadows = true;
	shadows::ShadowCamera camera;
	Bounds3f sceneBounds;
	// where the frame goes: the swapchain (its current image), and its frame, which the ui's pipelines are made for
	IRenderTarget* swapchain = nullptr;
	IRenderTarget* swapchainFrame = nullptr;
	// the exposure histograms (SHADER_TYPES_EXPOSURE_BINS per frame index), host visible: ComputeMain counts into the
	// frame's, which the host reads once the frame is done
	Buffer* exposureHistogram = nullptr;
	// the queue the frame is recorded for (its pool's secondary command buffers record the views), and its timeline
	Queue* graphicsQueue = nullptr;
	uint64_t graphicsTimeline = 0;
	// the ui: records its uploads (outside of a render pass), and its draws (in the swapchain's)
	std::function<void(CommandBufferHandle cmd)> prepareUi;
	std::function<void(CommandBufferHandle cmd)> drawUi;
};

// draws frames: the lights' shadows (see gfx/shadows.h), the main pass (in two phases with transmissive materials), the order independent transparency's
// resolve, tonemapping and the backdrop in ComputeMain, and the ui, as a FrameGraph whose transients are the main render
// target, the transparency's lists and the transmission texture.
class Renderer final
{
public:
	explicit Renderer(Device& device);
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

private:
	void InternalDrawMainPass(FrameGraph::PassContext& context, Pipeline& pipeline, const FrameInputs& inputs, MainPassPhase phase);

	FrameGraph myGraph;
	FrameGraph::ImageId myColor;
	FrameGraph::ImageId myDepth;
	FrameGraph::ImageId myTransmission;
	FrameGraph::ImageId mySwapchain;
	FrameGraph::ImageId myShadowAtlas;
	FrameGraph::BufferId myOitHeads;
	FrameGraph::BufferId myOitNodes;
	FrameGraph::BufferId myOitCounter;
	FrameGraph::BufferId myExposureHistogram;
	FrameGraph::PassId myTransmissionPass;
	FrameGraph::PassId myTransmissivePhase;
	FrameGraph::PassId myShadowPass;

	// per frame index: the shadows' views and each light's first (gShadowViews, gLightShadows), host visible
	std::vector<Buffer> myShadowViewBuffers;
	std::vector<Buffer> myLightShadowBuffers;
	shadows::ShadowPlan myShadowPlan;
	DrawList myShadowDrawList;

	// what Record's passes read of the frame they record
	const FrameInputs* myInputs = nullptr;
	Pipeline* myPipeline = nullptr;
	uint32_t myOitNodeCapacity = 0;

	// the frame's draw lists, per phase
	bool myTwoPhases = false;
	std::array<DrawList, 2> myDrawLists;
};

} // namespace gfx
