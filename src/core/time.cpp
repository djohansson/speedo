#include <core/time.h>

#include <algorithm>
#include <thread>

#ifdef _WIN32
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <windows.h>
#endif

namespace core
{

#ifdef _WIN32

namespace time
{

// the calling thread's high resolution timer (null where windows has none, before windows 10 1803)
struct Timer
{
	HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

	Timer() = default;
	Timer(const Timer&) = delete;
	Timer& operator=(const Timer&) = delete;
	~Timer()
	{
		if (handle != nullptr)
			CloseHandle(handle);
	}
};

} // namespace time

void SleepUntil(std::chrono::steady_clock::time_point time)
{
	thread_local time::Timer gTimer;

	auto remaining = time - std::chrono::steady_clock::now();
	if (remaining <= std::chrono::steady_clock::duration::zero())
		return;
	if (gTimer.handle == nullptr)
	{
		std::this_thread::sleep_until(time);
		return;
	}

	// relative, in 100 ns units
	LARGE_INTEGER dueTime;
	dueTime.QuadPart = -std::max<LONGLONG>(std::chrono::duration_cast<std::chrono::duration<LONGLONG, std::ratio<1, 10'000'000>>>(remaining).count(), 1);
	if (SetWaitableTimerEx(gTimer.handle, &dueTime, 0, nullptr, nullptr, nullptr, 0) != 0)
		WaitForSingleObject(gTimer.handle, INFINITE);
	else
		std::this_thread::sleep_until(time);
}

#else

void SleepUntil(std::chrono::steady_clock::time_point time)
{
	std::this_thread::sleep_until(time);
}

#endif

} // namespace core
