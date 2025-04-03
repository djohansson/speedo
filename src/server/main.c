#include "capi.h"

#include <core/assert.h>

#include <errno.h>
#include <signal.h> 
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cargs.h>
#include <ctrace/ctrace.h>
#if defined(SPEEDO_USE_MIMALLOC)
#include <mimalloc.h>
#endif

#if defined(__WINDOWS__)
#include <windows.h>
#else
#include <unistd.h>
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
static struct PathConfig gPaths = { NULL, NULL };
static volatile bool gIsInterrupted = false;

static void OnSignal(int signal)
{
	switch (signal)
	{	
	case SIGINT:
	case SIGTERM:
		gIsInterrupted = true;
		return;
	default:
		break;
	}

#if defined(__WINDOWS__)
	LOG_ERROR("Unhandled signal\n");
#else
	LOG_ERROR("Unhandled signal: %s\n", strsignal(signal));
#endif
}

static int _Sleep(const struct timespec* duration, struct timespec* remaining)
{
#if defined(__WINDOWS__)
	struct timespec start;
	timespec_get(&start, TIME_UTC);

	DWORD t = SleepEx(
		(DWORD)(duration->tv_sec * 1000 + duration->tv_nsec / 1000000 +
				(((duration->tv_nsec % 1000000) == 0) ? 0 : 1)), TRUE);

	if (t == 0)
	{
		return 0;
	}
	else
	{
		if (remaining != NULL)
		{
			timespec_get(remaining, TIME_UTC);
			remaining->tv_sec -= start.tv_sec;
			remaining->tv_nsec -= start.tv_nsec;
			if (remaining->tv_nsec < 0)
			{
				remaining->tv_nsec += 1000000000;
				remaining->tv_sec -= 1;
			}
		}

		return (t == WAIT_IO_COMPLETION) ? -1 : -2;
	}
#else
	int res = nanosleep(duration, remaining);
	if (res == 0)
	{
		return 0;
	}
	if (errno == EINTR)
	{
		return -1;
	}
	return -2;
#endif
}

int main(int argc, char* argv[], char* envp[])
{
#if defined(SPEEDO_USE_MIMALLOC)
	mi_version(); // if not called first thing in main(), malloc will not be redirected correctly on windows
#endif

	signal(SIGINT, OnSignal);
	signal(SIGTERM, OnSignal);
	signal(SIGILL, OnSignal);
	signal(SIGABRT, OnSignal);
	signal(SIGFPE, OnSignal);
	signal(SIGSEGV, OnSignal);

	ASSERT(argv != NULL);
	ASSERT(envp != NULL);

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
			printf("Usage: server [OPTION]...\n");
			cag_option_print(gCmdArgs, CAG_ARRAY_SIZE(gCmdArgs), stdout);
			return EXIT_SUCCESS;
		default:
			break;
		}
	}

	ServerCreate(&gPaths);

	fprintf(stdout, "Press Ctrl-C to quit\n");

	while (ServerExitRequested() && !gIsInterrupted)
	{
		_Sleep(&(struct timespec){.tv_nsec=100000000}, NULL);
	};

	ServerDestroy();

	return EXIT_SUCCESS;
}
