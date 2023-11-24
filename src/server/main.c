#include "capi.h"

#include <assert.h>
#include <signal.h> 
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mimalloc.h>

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

int main(int argc, char* argv[], char* env[])
{
	assert(argv != NULL);
	assert(env != NULL);

	atexit(onExit);
	
	signal(SIGINT, onSignal);
	signal(SIGTERM, onSignal);

	printf("mi_version(): %d\n", mi_version());

	server_create(
		"./",
		getCmdOption(argv, argv + argc, "(-r)"),
		getCmdOption(argv, argv + argc, "(-u)"));

	while (server_tick() && !g_isInterrupted) {};

	return EXIT_SUCCESS;
}
