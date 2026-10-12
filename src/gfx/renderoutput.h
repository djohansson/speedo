#pragma once

#include <gfx/framegraph.h>
#include <gfx/gpu.h>

#include <platform/capi.h>

#include <cstdint>
#include <memory>

namespace gfx
{

struct FrameInputs;

// what the passes that end a frame (RenderOutput::AddFinalPasses) see of the frame being recorded (see Renderer::Record),
// while it is
struct RecordingFrame
{
	const FrameInputs* inputs = nullptr;
	Pipeline* pipeline = nullptr;
};

// where a scene's frames go (see Renderer): ComputeMain writes each into the output's target as a storage image (frame
// index by frame index), then the output's final passes end the frame. its owner (re)creates it, then has the scene
// resize after it (Scene::Resize)
class RenderOutput
{
public:
	virtual ~RenderOutput() = default;

	// the extent frames are drawn at
	[[nodiscard]] virtual rhi::Extent2d GetExtent() const = 0;

	// how many frame indices there are, and each one's storage view: bound up front, since a dirty descriptor set is
	// updated as a whole (see Scene)
	[[nodiscard]] virtual uint32_t GetFrameCount() const = 0;
	[[nodiscard]] virtual ImageViewHandle GetFrameView(uint32_t frameIndex) const = 0;

	// what the frame of frameIndex is drawn into
	[[nodiscard]] virtual IRenderTarget& GetTarget(uint32_t frameIndex) = 0;

	// puts a (re)created output in the state its first frame expects. with the gpu idle, outside a render pass
	virtual void Prepare(CommandBufferHandle cmd) = 0;

	// adds the passes that end a frame to graph, after ComputeMain wrote output (the target, bound each frame)
	virtual void AddFinalPasses(FrameGraph& graph, FrameGraph::ImageId output, const RecordingFrame& frame) = 0;
};

// a window's swapchain: the ui is drawn over each frame (FrameInputs::prepareUi and drawUi), which is then presented
class SwapchainOutput final : public RenderOutput
{
public:
	// the swapchain is looked up each time (the rhi recreates it with the window)
	SwapchainOutput(RHI& rhi, WindowHandle window);

	[[nodiscard]] rhi::Extent2d GetExtent() const final;
	[[nodiscard]] uint32_t GetFrameCount() const final;
	[[nodiscard]] ImageViewHandle GetFrameView(uint32_t frameIndex) const final;
	[[nodiscard]] IRenderTarget& GetTarget(uint32_t frameIndex) final;
	void Prepare(CommandBufferHandle cmd) final;
	void AddFinalPasses(FrameGraph& graph, FrameGraph::ImageId output, const RecordingFrame& frame) final;

private:
	[[nodiscard]] Swapchain& InternalSwapchain() const;

	RHI& myRHI;
	WindowHandle myWindow;
};

// an image, left for sampling (an off-screen view, e.g. shown in the ui): RGBA8 unorm, srgb encoded as the swapchain's
// image is. one serves every frame index: the frames are serial on the graphics queue, and the frame graph orders each
// frame's write after the previous frame's sampling
class ImageOutput final : public RenderOutput
{
public:
	ImageOutput(Device& device, rhi::Extent2d extent);

	// recreates the image at extent: its view changes. with the gpu done with the previous one
	void Resize(rhi::Extent2d extent);

	// the view the ui samples, in kShaderReadOnly once a frame is done (and before the first: see Prepare)
	[[nodiscard]] ImageViewHandle GetView() const;
	// the image itself (e.g. to read it back, between frames, leaving it in kShaderReadOnly)
	[[nodiscard]] Image& GetImage() noexcept { return *myImage; }

	[[nodiscard]] rhi::Extent2d GetExtent() const final { return myExtent; }
	[[nodiscard]] uint32_t GetFrameCount() const final;
	[[nodiscard]] ImageViewHandle GetFrameView(uint32_t frameIndex) const final;
	[[nodiscard]] IRenderTarget& GetTarget(uint32_t frameIndex) final;
	void Prepare(CommandBufferHandle cmd) final;
	void AddFinalPasses(FrameGraph& graph, FrameGraph::ImageId output, const RecordingFrame& frame) final;

private:
	Device& myDevice;
	rhi::Extent2d myExtent{};
	std::shared_ptr<Image> myImage;
	std::unique_ptr<RenderImageSet> myTarget;
};

} // namespace gfx
