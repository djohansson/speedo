#pragma once

#include <gfx/gpu.h>

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace gfx
{

// diagnostic: what the device's ray queries do (SPEEDO_RAY_QUERY_WATCH=1, where the device has them), for a crash report
// to tell after the gpu hung (KosmicKrisp's ray queries beside other processes' gpu work restarted the gpu, see
// CLAUDE.md). the shaders, compiled with BVH_RAY_QUERY_WATCH, count the queries begun and ended in gRayQueryWatch and
// record each in a slot while it is in flight (see RAY_QUERY_WATCH_BEGUN); the renderer gives each compute dispatch a
// breadcrumb of its own within its pass's, so that a report also tells which kernel was in flight
bool RayQueryWatchOn();

// diagnostic: rays traced again (SPEEDO_RAY_QUERY_REPLAY=<file>, which turns the watch on too): a ray a line, its
// origin's x, y and z, its direction's, its tmax, and whether it was any hit and moving (0 or 1), as a crash report's
// watch lists them (lines from # on are comments). the renderer's Ray Query Replay pass traces each of them
// SPEEDO_RAY_QUERY_REPLAY_REPEAT times a frame (64), so that a ray that hangs the gpu by itself hangs it alone. as
// gRayQueryReplay holds them: their count, then two float4 a ray (see ComputeRayQueryReplay); empty without it
const std::vector<glm::vec4>& RayQueryReplayData();
uint32_t RayQueryReplayRayCount();
uint32_t RayQueryReplayRepeatCount();

// gRayQueryWatch: host visible and coherent, kept mapped, a section of a crash report's ("rayQueryWatch<n>", the n-th
// made), and its counts printed when it is destroyed (where nothing hung, every query begun ended)
class RayQueryWatch final
{
public:
	explicit RayQueryWatch(Device& device);
	~RayQueryWatch();

	RayQueryWatch(const RayQueryWatch&) = delete;
	RayQueryWatch& operator=(const RayQueryWatch&) = delete;

	[[nodiscard]] const Buffer& GetBuffer() const noexcept { return myBuffer; }
	// gRayQueryReplay: the rays replayed, or a count of none
	[[nodiscard]] const Buffer& GetReplayBuffer() const noexcept { return myReplayBuffer; }

private:
	Buffer myBuffer;
	std::span<std::byte> myMemory;
	std::string mySection;
	Buffer myReplayBuffer;
};

// what a watch's words say, as json: the counts, and the queries in flight
std::string RayQueryWatchJson(std::span<const std::byte> memory);

} // namespace gfx
