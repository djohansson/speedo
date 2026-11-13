#pragma once

#include <gfx/gpu.h>

#include <core/task.h>

#include <cstdint>
#include <memory>
#include <vector>

#include <imgui.h>

namespace gfx::inline SPEEDO_GRAPHICS_API_NAMESPACE
{

// draws imgui's frames (see platform::ImGuiLayer) with rhi, through a pipeline and a layout ("ImGui", imgui.slang) of
// its own: its vertices pulled from a buffer per frame slot, and its textures, imgui's (its font atlas, created,
// uploaded and destroyed as imgui asks) and the caller's (AddTexture), slots of one array. the imgui context must
// outlive it.
//
// threads: the ui thread calls UpdateTextures, AddTexture and RemoveTexture, the draw thread PrepareFrame and Draw. ui
// frames are numbered (their sequence) by the caller, which hands them to the draw thread in order.
class ImGuiRenderer final
{
public:
	explicit ImGuiRenderer(Device& device);
	ImGuiRenderer(const ImGuiRenderer&) = delete;
	ImGuiRenderer(ImGuiRenderer&&) noexcept = delete;
	// destroys all textures, so the gpu must be done with them (idle, with all timeline callbacks run)
	~ImGuiRenderer();

	ImGuiRenderer& operator=(const ImGuiRenderer&) = delete;
	ImGuiRenderer& operator=(ImGuiRenderer&&) noexcept = delete;

	// creates, updates or destroys imgui's textures as it asks, as of ui frame sequence. ui thread, after ImGui::Render
	void UpdateTextures(uint64_t sequence);
	// records the texture uploads queued so far into cmd (outside of a render pass), and copies drawData's vertices and
	// indices into the frame slot's buffers. what no frame from drawnSequence (drawData's) on draws is released from a task
	// added to callbacks, which must run once the gpu has completed the submission of cmd. draw thread, before Draw, once
	// the frame slot's last frame is done
	void PrepareFrame(
		CommandBufferHandle cmd,
		const ImDrawData& drawData,
		uint64_t drawnSequence,
		uint32_t frameIndex,
		std::vector<core::TaskHandle>& callbacks);
	// records drawData (as PrepareFrame copied it) into cmd, inside target, begun on one of its frames. draw thread
	void Draw(CommandBufferHandle cmd, const ImDrawData& drawData, uint32_t frameIndex, IRenderTarget& target);

	// an image the caller owns, shown in imgui (ImGui::Image) through the texture id returned: view must be in
	// kShaderReadOnly whenever a frame drawing it runs, and outlive the frames that do. RemoveTexture releases the id: the
	// ui frame being built must not draw it, and the frames before may still be drawn (it is released once the gpu is done
	// with them, see PrepareFrame). an id is a slot and a generation of it, so a draw of one released (or not the
	// renderer's) fails rather than drawing what the slot holds since. ui thread
	[[nodiscard]] ImTextureID AddTexture(ImageViewHandle view);
	void RemoveTexture(ImTextureID id);

private:
	struct State;
	std::unique_ptr<State> myState;
};

} // namespace gfx::inline SPEEDO_GRAPHICS_API_NAMESPACE
