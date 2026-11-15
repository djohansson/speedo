#pragma once

#include <gfx/gpu.h>
#include <gfx/renderoutput.h>

#include <core/task.h>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace client::inline SPEEDO_GRAPHICS_API_NAMESPACE
{

// what a frame's captures read back: the swapchain's image of outputIndex, or headless (no swapchain) the image output's
struct FrameCaptureTarget
{
	gfx::Device& device;
	gfx::Swapchain* swapchain = nullptr;
	gfx::ImageOutput* imageOutput = nullptr;
	rhi::Extent2d extent{}; // the output's
	uint32_t outputIndex = 0;
	std::optional<size_t> placedPose; // the pose of SPEEDO_CAMERA_PATH placed this frame, if any (Views::GetPlacedPose)
};

// records the captures this frame asks for into cmd, each a read back of the frame into a bitmap file, written by a
// graphics timeline callback (added to callbacks): SPEEDO_CAPTURE's (the ui's request, or with SPEEDO_REPEATABLE the
// frame counted from the sequences' restart, see gRepeatableCaptureFrame), and SPEEDO_CAPTURE_PATH's (a frame drawing a
// pose of SPEEDO_CAMERA_PATH for the first time). draw thread, in the frame's command buffer after the scene's
void RecordFrameCaptures(gfx::CommandBufferHandle cmd, const FrameCaptureTarget& target, std::vector<core::TaskHandle>& callbacks);

// a 32 bit bitmap file of pixels (rgba, or bgra), top down (a negative height), as bgra
[[nodiscard]] std::vector<std::byte> EncodeBmp(std::span<const std::byte> pixels, rhi::Extent2d extent, bool bgra);

} // namespace client::inline SPEEDO_GRAPHICS_API_NAMESPACE
