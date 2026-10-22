#pragma once

#include <chrono>

namespace core
{

// sleeps the calling thread until time, to within about half a millisecond. std::this_thread::sleep_until is that
// precise on macos and linux, but on windows it rounds up to the system timer's resolution, 15.6 ms unless a process
// raises it (and windows 11 ignores a raise from a process whose windows are all hidden): the client's tick, paced to a
// millisecond, ran at 64 hz, so the camera and the ui moved at 64 hz while frames were drawn at hundreds. there it waits
// on a high resolution waitable timer (CREATE_WAITABLE_TIMER_HIGH_RESOLUTION), one per thread
void SleepUntil(std::chrono::steady_clock::time_point time);

} // namespace core
