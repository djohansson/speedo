#include <core/runtimings.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <print>
#include <string_view>
#include <utility>
#include <vector>

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

namespace runtimings
{

// about when the process started (its static initialization, before main)
const auto gStart = std::chrono::steady_clock::now();
const auto gStartSystem = std::chrono::system_clock::now();

const bool gEnabled = []
{
	const char* value = std::getenv("SPEEDO_RUN_TIMINGS");
	return value != nullptr && std::string_view(value) == "1";
}();

struct Milestone
{
	std::chrono::steady_clock::time_point time;
	std::optional<uint64_t> frames;
	std::string what;
};

std::mutex gMutex;
std::vector<Milestone> gMilestones; // guarded by gMutex
std::atomic<const std::atomic_uint64_t*> gFrames{nullptr};

// how long before gStartSystem the process was created: the loader's time, the libraries' and their initialization
std::optional<double> ProcessCreatedSecondsBefore()
{
#ifdef _WIN32
	FILETIME creation{};
	FILETIME exit{};
	FILETIME kernel{};
	FILETIME user{};
	if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) == 0)
		return std::nullopt;
	// 100 ns units since 1601-01-01, which is 11644473600 s before the unix epoch
	auto ticks = (static_cast<uint64_t>(creation.dwHighDateTime) << 32U) | creation.dwLowDateTime;
	double created = static_cast<double>(ticks) * 1e-7 - 11644473600.0;
	double started = std::chrono::duration<double>(gStartSystem.time_since_epoch()).count();
	return started - created;
#else
	return std::nullopt;
#endif
}

} // namespace runtimings

bool RunTimings() noexcept
{
	return runtimings::gEnabled;
}

void RunTiming(std::string what)
{
	using namespace runtimings;

	if (!gEnabled)
		return;
	auto now = std::chrono::steady_clock::now();
	std::optional<uint64_t> frames;
	if (const auto* counter = gFrames.load(std::memory_order_acquire))
		frames = counter->load(std::memory_order_relaxed);
	std::scoped_lock lock(gMutex);
	gMilestones.push_back({now, frames, std::move(what)});
}

void SetRunTimingFrames(const std::atomic_uint64_t* frames) noexcept
{
	runtimings::gFrames.store(frames, std::memory_order_release);
}

void PrintRunTimings()
{
	using namespace runtimings;

	if (!gEnabled)
		return;
	std::scoped_lock lock(gMutex);
	std::println("run timings (seconds from the process's static initialization, the step since the last, frames drawn):");
	if (auto before = ProcessCreatedSecondsBefore())
		std::println("  {:8.3f}            process created", -*before);
	auto last = gStart;
	for (const auto& milestone : gMilestones)
	{
		auto at = std::chrono::duration<double>(milestone.time - gStart).count();
		auto step = std::chrono::duration<double>(milestone.time - last).count();
		last = milestone.time;
		if (milestone.frames)
			std::println("  {:8.3f} {:+8.3f} {:6} {}", at, step, *milestone.frames, milestone.what);
		else
			std::println("  {:8.3f} {:+8.3f}        {}", at, step, milestone.what);
	}
	std::fflush(stdout);
}

} // namespace core
