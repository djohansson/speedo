#pragma once

#include <rhi/types.h>

#include <core/task.h>

#include <cstdint>
#include <memory>
#include <vector>

struct ImDrawData;

namespace rhi
{

template <GraphicsApi G>
class Device;

template <GraphicsApi G>
class Swapchain;

template <GraphicsApi G>
class Queue;

// draws imgui into a swapchain's frames: imgui's renderer backend for the graphics api, plus imgui's textures, which it
// creates, uploads and destroys itself (rather than leaving that to the backend, which would submit to, and wait for,
// the queue the draw thread uses). the imgui context (and the platform backend) belong to the caller, and must
// outlive it.
//
// threads: the ui thread (one at a time) calls NewFrame and UpdateTextures, the draw thread PrepareFrame and Render.
// ui frames are numbered (sequence) by the caller, which hands them to the draw thread in order.
template <GraphicsApi G>
class ImGuiRenderer final
{
public:
	// framesInFlight: how many frames the queue may have in flight, so how many sets of buffers imgui needs
	ImGuiRenderer(Device<G>& device, Swapchain<G>& swapchain, Queue<G>& queue, uint32_t framesInFlight);
	ImGuiRenderer(const ImGuiRenderer&) = delete;
	ImGuiRenderer(ImGuiRenderer&&) noexcept = delete;
	// destroys all textures, so the gpu must be done with them (idle, with all timeline callbacks run)
	~ImGuiRenderer();

	ImGuiRenderer& operator=(const ImGuiRenderer&) = delete;
	ImGuiRenderer& operator=(ImGuiRenderer&&) noexcept = delete;

	// call on the ui thread, before ImGui::NewFrame
	void NewFrame();
	// creates, updates or destroys imgui's textures as it requests, as of ui frame sequence. call on the ui thread,
	// after ImGui::Render.
	void UpdateTextures(uint64_t sequence);
	// records the texture uploads queued so far into cmd (outside of a render pass). textures no longer drawn from ui
	// frame drawnSequence on are destroyed from a task added to callbacks, which must run once the gpu has completed
	// the submission of cmd. call on the draw thread, before Render.
	void PrepareFrame(CommandBufferHandle<G> cmd, uint64_t drawnSequence, std::vector<core::TaskHandle>& callbacks);
	// records drawData into cmd, inside a render target begun on one of the swapchain's frames. draw thread.
	void Render(ImDrawData& drawData, CommandBufferHandle<G> cmd);

private:
	struct State;
	std::unique_ptr<State> myState;
};

} // namespace rhi
