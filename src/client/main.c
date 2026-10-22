#include <core/assert.h>
#include <core/capi.h>

#include <client/capi.h>

#include <platform/capi.h>
#include <rhi/capi.h>

#include <signal.h> 
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cargs.h>
#include <ctrace/ctrace.h>
#if defined(SPEEDO_USE_MIMALLOC)
#include <mimalloc.h>
#endif
#if !defined(__WINDOWS__)
#	include <pthread.h>
#	include <unistd.h>
#endif

static struct cag_option gCmdArgs[] =
{
	{
		.identifier = 'r',
		.access_letters = "r",
		.access_name = "resourcePath",
		.value_name = "VALUE",
		.description = "Path to resource directory"
	},
	{
		.identifier = 'u',
		.access_letters = "u",
		.access_name = "userProfilePath",
		.value_name = "VALUE",
		.description = "Path to user profile directory"
	},
	{
		.identifier = 'h',
		.access_letters = "h?",
		.access_name = "help",
		.description = "Shows the command help"
	}
};
static struct PathConfig gPaths;
static atomic_bool gIsInterrupted = false;

static void OnInterrupt(void)
{
	atomic_store(&gIsInterrupted, true);

	// the main loop sleeps in PlatformWaitEvents() until the next window event, so wake it up
	PlatformWakeMain();
}

#ifndef __WINDOWS__
// PlatformWakeMain() is not async-signal-safe, so the SIGINT handler only writes to a pipe (which is),
// and this thread forwards the wakeup to the platform from a regular thread context (self-pipe trick).
static int gInterruptPipe[2] = {-1, -1};

static void* InterruptThreadMain(void* arg)
{
	(void)arg;

	char byte;
	while (read(gInterruptPipe[0], &byte, 1) == 1)
		OnInterrupt();

	return NULL;
}

static void StartInterruptThread(void)
{
	ENSURE(pipe(gInterruptPipe) == 0);

	pthread_t thread;
	ENSURE(pthread_create(&thread, NULL, InterruptThreadMain, NULL) == 0);
	ENSURE(pthread_detach(thread) == 0);
}
#endif

static void OnSignal(int sig)
{
	switch (sig)
	{	
	case SIGINT:
#if defined(__WINDOWS__)
		OnInterrupt(); // runs on a separate thread on windows, so calling into the platform is fine
#else
		atomic_store(&gIsInterrupted, true);
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
#if defined(__WINDOWS__)
		LOG_ERROR("Unhandled signal\n");
#else
		LOG_ERROR("Unhandled signal: %s\n", strsignal(sig));
#endif
		break;
	}

	// re-raise with the default action, so the process actually terminates. returning from e.g. SIGSEGV would
	// re-execute the faulting instruction and loop in this handler forever.
	signal(sig, SIG_DFL);
	raise(sig);
}

int main(int argc, char* argv[], char* envp[])
{
#if defined(SPEEDO_USE_MIMALLOC)
	mi_version(); // if not called first thing in main(), malloc will not be redirected correctly on windows
#endif

#if !defined(__WINDOWS__)
	StartInterruptThread();
#endif
	signal(SIGINT, OnSignal);
	signal(SIGTERM, OnSignal);
#if !defined(SPEEDO_SANITIZE)
	// a sanitizer reports these itself. ours allocates (the stack trace), which deadlocked tsan when its runtime faulted
	// holding its own locks
	signal(SIGILL, OnSignal);
	signal(SIGABRT, OnSignal);
	signal(SIGFPE, OnSignal);
	signal(SIGSEGV, OnSignal);
#endif

	ENSURE(argv != NULL);
	ENSURE(envp != NULL);

	cag_option_context cagContext;
	cag_option_init(&cagContext, gCmdArgs, CAG_ARRAY_SIZE(gCmdArgs), argc, argv);
	
	while (cag_option_fetch(&cagContext))
	{
		switch (cag_option_get_identifier(&cagContext))
		{
		case 'u':
			gPaths.userProfilePath = cag_option_get_value(&cagContext);
			break;
		case 'r':
			gPaths.resourcePath = cag_option_get_value(&cagContext);
			break;
		case 'h':
			printf("Usage: client [OPTION]...\n");
			cag_option_print(gCmdArgs, CAG_ARRAY_SIZE(gCmdArgs), stdout);
			return EXIT_SUCCESS;
		default:
			break;
		}
	}

	// the window system first: the client makes its window
	PlatformInit();
	ClientCreate(&gPaths);
	// with a timeout: a wake up (for mainCalls) that something peeked at before the wait looked is no new message to it,
	// and the main thread slept on until the next input (a fullscreen switch waiting for its main call stalled, twice,
	// until then)
	while (PlatformWaitEvents(0.1) && ClientMain() && !atomic_load(&gIsInterrupted))
		;
	ClientDestroy();
	PlatformTerminate();

	return EXIT_SUCCESS;
}
