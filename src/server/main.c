#include "capi.h"

#include <core/assert.h>

#include <signal.h> 
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cargs.h>
#include <ctrace/ctrace.h>
#if defined(SPEEDO_USE_MIMALLOC)
#include <mimalloc.h>
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

#if !defined(__WINDOWS__)
	fprintf(stderr, "Unhandled signal: %s\n", strsignal(signal));
#endif

	ctrace_stacktrace trace = ctrace_generate_trace(0, 64);
	ctrace_print_stacktrace(&trace, stderr, 1);
}

int main(int argc, char* argv[], char* envp[])
{
#if defined(SPEEDO_USE_MIMALLOC)
	mi_version(); // if not called first thing in main(), malloc will not be redirected correctly on windows
#endif
	
	signal(SIGINT, &OnSignal);
	signal(SIGTERM, &OnSignal);
	signal(SIGILL, &OnSignal);
	signal(SIGABRT, &OnSignal);
	signal(SIGFPE, &OnSignal);
	signal(SIGSEGV, &OnSignal);

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

	CreateServer(&gPaths);

	while (OnEventServer() && !gIsInterrupted) {};

	DestroyServer();

	return EXIT_SUCCESS;
}
