// what an executable's main sets up (see core/capi.h): its signal handlers, how an interrupt reaches its main loop, its
// arguments, and its main loop's sleep. the client's and the server's main.c each had their own copy of these

#include <core/assert.h>//NOLINT(modernize-deprecated-headers)
#include <core/capi.h>
#include <core/time.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <print>

#include <cargs.h>

#if !defined(_WIN32)
#	include <pthread.h>
#	include <unistd.h>
#endif

namespace core::process
{

namespace detail
{

std::atomic_bool gInterrupted = false;
std::atomic<InterruptFunc> gOnInterrupt = nullptr;

void OnInterrupt()
{
	gInterrupted = true;
	if (auto onInterrupt = gOnInterrupt.load())
		onInterrupt();
}

#if !defined(_WIN32)
// the interrupt callback (e.g. a window system's wake up) needn't be async signal safe, so the SIGINT handler only writes
// to a pipe (which is), and this thread calls it from a regular thread's context (the self-pipe trick)
int gInterruptPipe[2] = {-1, -1};

void* InterruptThreadMain(void* /*arg*/)
{
	char byte = 0;
	while (read(gInterruptPipe[0], &byte, 1) == 1)
		OnInterrupt();
	return nullptr;
}
#endif

void OnSignal(int sig)
{
	switch (sig)
	{
	case SIGINT:
#if defined(_WIN32)
		OnInterrupt(); // runs on a thread of its own on windows, so calling the callback is fine
#else
		gInterrupted = true;
		(void)!write(gInterruptPipe[1], "", 1);
#endif
		return;
	case SIGTERM:
		LOG_ERROR("Program terminated.");
		break;
	case SIGABRT:
		LOG_ERROR("Program aborted.");
		break;
	default:
#if defined(_WIN32)
		LOG_ERROR("Unhandled signal");
#else
		LOG_ERROR("Unhandled signal: {}", strsignal(sig));
#endif
		break;
	}

	// re-raised with the default action, so the process actually terminates. returning from e.g. SIGSEGV would
	// re-execute the faulting instruction and loop in this handler forever.
	std::signal(sig, SIG_DFL);
	std::raise(sig);
}

} // namespace detail

} // namespace core::process

void InstallSignalHandlers(InterruptFunc onInterrupt)
{
	using namespace core::process::detail;

	gOnInterrupt = onInterrupt;

#if !defined(_WIN32)
	if (gInterruptPipe[0] < 0)
	{
		if (pipe(gInterruptPipe) != 0)
			std::abort();
		pthread_t thread{};
		if (pthread_create(&thread, nullptr, InterruptThreadMain, nullptr) != 0 || pthread_detach(thread) != 0)
			std::abort();
	}
#endif

	std::signal(SIGINT, OnSignal);
	std::signal(SIGTERM, OnSignal);
#if !defined(SPEEDO_SANITIZE)
	// a sanitizer reports these itself. ours allocates (the stack trace), which deadlocked tsan when its runtime faulted
	// holding its own locks
	std::signal(SIGILL, OnSignal);
	std::signal(SIGABRT, OnSignal);
	std::signal(SIGFPE, OnSignal);
	std::signal(SIGSEGV, OnSignal);
#endif
}

bool IsInterrupted(void)
{
	return core::process::detail::gInterrupted.load();
}

bool ParseArguments(int argc, char* argv[], const char* programName, struct PathConfig* paths)
{
	static cag_option gOptions[] = {
		{.identifier = 'r', .access_letters = "r", .access_name = "resourcePath", .value_name = "VALUE", .description = "Path to resource directory"},
		{.identifier = 'u', .access_letters = "u", .access_name = "userProfilePath", .value_name = "VALUE", .description = "Path to user profile directory"},
		{.identifier = 'h', .access_letters = "h?", .access_name = "help", .value_name = nullptr, .description = "Shows the command help"},
	};

	cag_option_context context;
	cag_option_init(&context, gOptions, CAG_ARRAY_SIZE(gOptions), argc, argv);
	while (cag_option_fetch(&context))
	{
		switch (cag_option_get_identifier(&context))
		{
		case 'u':
			paths->userProfilePath = cag_option_get_value(&context);
			break;
		case 'r':
			paths->resourcePath = cag_option_get_value(&context);
			break;
		case 'h':
			std::println("Usage: {} [OPTION]...", programName);
			std::fflush(stdout);
			cag_option_print(gOptions, CAG_ARRAY_SIZE(gOptions), stdout);
			return false;
		default:
			break;
		}
	}
	return true;
}

void SleepSeconds(double seconds)
{
	core::SleepUntil(std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds)));
}
