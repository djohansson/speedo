#pragma once

#include <gfx/gpu.h>
#include <gfx/imgui_extra.h>

#include <platform/window.h>

#include <core/task.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include <imgui.h>

namespace gfx
{

// the user interface's imgui: its context (fonts and style at the window's content scale), bound to the window
// (platform::imgui) and drawn by rhi's ImGuiRenderer. a frame is built on the ui thread (BeginFrame, the application's
// imgui calls, EndFrame) and drawn on the draw thread (PrepareFrame, Draw), handed over through a triple buffer:
// EndFrame snapshots into its write frame and publishes it as the pending one, PrepareFrame takes the pending frame as
// its read frame. each frame's snapshot owns its draw lists, so neither thread touches the other's.
class ImGuiLayer final
{
public:
	// with the graphics queue's lock held: the renderer allows graphicsQueueCount frames in flight. iniSettings: imgui's
	// saved window layout
	ImGuiLayer(
		const platform::Window& window,
		Swapchain& swapchain,
		RHI& rhi,
		Queue& graphicsQueue,
		uint32_t graphicsQueueCount,
		std::string_view imguiIniSettings);
	~ImGuiLayer();

	ImGuiLayer(const ImGuiLayer&) = delete;
	ImGuiLayer& operator=(const ImGuiLayer&) = delete;

	// starts a frame (polling the window's input), and ends it: renders it, updates imgui's textures and publishes the
	// frame for the draw thread. ui thread.
	void BeginFrame();
	void EndFrame();

	// takes the latest frame published by EndFrame and records the texture uploads it depends on into `cmd` (outside of
	// a render pass). textures that are no longer drawn are destroyed from a task added to `callbacks`, which must run
	// once the gpu has completed the submission of `cmd`. draw thread, before Draw.
	void PrepareFrame(CommandBufferHandle cmd, std::vector<core::TaskHandle>& callbacks);
	// records the frame's draws into cmd, inside the swapchain frame's render target. draw thread.
	void Draw(CommandBufferHandle cmd);

private:
	struct Frame
	{
		imgui_extra::ImDrawDataSnapshot snapshot;
		ImDrawData drawData;
		uint64_t sequence = 0; // EndFrame count when published
	};
	static constexpr uint8_t kFrameFresh = 0x80; // set on myPendingFrame when published but not yet taken

	std::array<Frame, 3> myFrames;
	uint8_t myWriteFrame = 0; // ui thread only
	uint8_t myReadFrame = 1; // draw thread only
	std::atomic_uint8_t myPendingFrame = 2;
	uint64_t myFrameSequence = 0; // ui thread only

	// imgui's renderer, see ImGuiRenderer for which thread calls what
	std::unique_ptr<ImGuiRenderer> myRenderer;
};

} // namespace gfx
