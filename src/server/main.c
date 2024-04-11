#include "capi.h"

#include <assert.h>
#include <signal.h> 
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cargs.h>

#include <mimalloc.h>

static struct cag_option g_cmdArgs[] =
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
		.access_letters = "h",
		.access_name = "help",
		.description = "Shows the command help"
	}
};

static PathConfig g_paths = { NULL, NULL };

static volatile bool g_isInterrupted = false;

void onExit(void) 
{
	server_destroy();
}

void onSignal(int signal)
{
	if (signal == SIGINT || signal == SIGTERM)
		g_isInterrupted = true;
}

const char* getCmdOption(char** begin, char** end, const char* option)
{
	assert(begin != NULL);
	assert(end != NULL);
	assert(option != NULL);

	while (begin != end)
	{
		if (strcmp(*begin++, option) == 0)
			return *begin;
	}

	return NULL;
}

int main(int argc, char* argv[], char* envp[])
{
	assert(argv != NULL);
	assert(envp != NULL);

	signal(SIGINT, onSignal);
	signal(SIGTERM, onSignal);

	cag_option_context cagContext;
	cag_option_init(&cagContext, g_cmdArgs, CAG_ARRAY_SIZE(g_cmdArgs), argc, argv);
	
	while (cag_option_fetch(&cagContext))
	{
		switch (cag_option_get_identifier(&cagContext))
		{
		case 'u':
			g_paths.userProfilePath = cag_option_get_value(&cagContext);
			break;
		case 'r':
			g_paths.resourcePath = cag_option_get_value(&cagContext);
			break;
		case 'h':
			printf("Usage: server [OPTION]...\n");
			cag_option_print(g_cmdArgs, CAG_ARRAY_SIZE(g_cmdArgs), stdout);
			return EXIT_SUCCESS;
		default:
			break;
		}
	}

	(void)mi_version();

	server_create(&g_paths);

	atexit(onExit);

	while (server_tick() && !g_isInterrupted) {};

	return EXIT_SUCCESS;
}
