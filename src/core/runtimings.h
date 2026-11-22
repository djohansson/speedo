#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace core
{

// SPEEDO_RUN_TIMINGS=1: a run's milestones (the application made, the shaders loaded, the loads done, the exit's count
// started, the capture saved, the shutdown's steps), each with its time from the process's start and the frames drawn
// by then, printed as the run ends (PrintRunTimings): where an automated run's time goes, a capture's in particular
[[nodiscard]] bool RunTimings() noexcept;
// a milestone, from any thread (nothing unless SPEEDO_RUN_TIMINGS=1)
void RunTiming(std::string what);
// the drawn frames each milestone notes (the client's count, set once)
void SetRunTimingFrames(const std::atomic_uint64_t* frames) noexcept;
// the milestones so far, and how long before the process's static initialization it was created (where the platform
// says)
void PrintRunTimings();

} // namespace core
