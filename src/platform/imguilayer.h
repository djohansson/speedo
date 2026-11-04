#pragma once

#include <platform/imgui_extra.h>
#include <platform/window.h>

#include <core/std_extra.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <imgui.h>

namespace platform
{

// a frosted panel of the ui (see ImGuiLayer::TakeFrame): a top level window's rectangle (left, top, right, bottom) and
// its corners' radius, in pixels of the framebuffer, and how opaque it is (see ImGuiLayer::Fade)
struct ImGuiPanel
{
	std::array<float, 4> rect{};
	float radius = 0.0F;
	float opacity = 1.0F;
};

// a frame of the ui, for the draw thread (see ImGuiLayer::TakeFrame): imgui's draw data, its sequence (the EndFrame
// count when it was published) and its frosted panels
struct ImGuiFrame
{
	const ImDrawData& drawData;
	uint64_t sequence = 0;
	std::span<const ImGuiPanel> panels;
};

// the user interface's imgui, all but its drawing: its context (fonts and style at the window's content scale), bound
// to the window (platform::imgui), and its frames. a frame is built on the ui thread (BeginFrame, the application's imgui
// calls, EndFrame) and drawn by a renderer on the draw thread (TakeFrame), handed over through a triple buffer: EndFrame
// snapshots into its write frame and publishes it as the pending one, TakeFrame takes the pending frame as its read
// frame. each frame's snapshot owns its draw lists, so neither thread touches the other's.
class ImGuiLayer final
{
public:
	// iniSettings: imgui's saved window layout
	ImGuiLayer(const Window& window, std::string_view iniSettings);
	~ImGuiLayer();

	ImGuiLayer(const ImGuiLayer&) = delete;
	ImGuiLayer& operator=(const ImGuiLayer&) = delete;

	// starts a frame (polling the window's input), and ends it: renders it, has the renderer handle imgui's texture
	// requests as of the frame's sequence (updateTextures, before the snapshot, which resolves the textures to their ids)
	// and publishes the frame for the draw thread. ui thread.
	void BeginFrame();
	void EndFrame(core::std_extra::function_ref<void(uint64_t sequence)> updateTextures);

	// fades the windows begun from here on this frame, and their frosted backdrops, to opacity (0: invisible, 1: as
	// styled), through imgui's style alpha: the ones begun before keep theirs (overlays that stay, such as progress bars).
	// they still take input. ui thread, between BeginFrame and EndFrame
	void Fade(float opacity);
	// whether the last frame (EndFrame) showed a window begun after Fade other than the main menu bar: a dialog, a menu, a
	// popup or a tooltip. ui thread
	[[nodiscard]] bool ShowedFadingWindows() const noexcept { return myShowedFadingWindows; }

	// takes the latest frame published by EndFrame (else keeps the one before), to draw: its draw data, sequence and
	// frosted panels (the main menu bar and the menus, popups and tooltips open, whose backdrop the renderer blurs). valid
	// until the next TakeFrame. draw thread
	[[nodiscard]] ImGuiFrame TakeFrame();

private:
	struct Frame
	{
		imgui_extra::ImDrawDataSnapshot snapshot;
		ImDrawData drawData;
		uint64_t sequence = 0; // EndFrame count when published
		std::vector<ImGuiPanel> panels; // see TakeFrame
	};
	static constexpr uint8_t kFrameFresh = 0x80; // set on myPendingFrame when published but not yet taken

	std::array<Frame, 3> myFrames;
	uint8_t myWriteFrame = 0; // ui thread only
	uint8_t myReadFrame = 1; // draw thread only
	std::atomic_uint8_t myPendingFrame = 2;
	uint64_t myFrameSequence = 0; // ui thread only
	// the frame's fade (see Fade): its opacity, and the begin order (see ImGuiWindow::BeginOrderWithinContext) of the
	// first window it applies to. ui thread only
	float myFadeOpacity = 1.0F;
	int myFadeFirstWindow = 0;
	bool myShowedFadingWindows = false;
};

} // namespace platform
