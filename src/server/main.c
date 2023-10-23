#include "capi.h"

#include <assert.h>
#include <signal.h> 
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mimalloc.h>

static volatile bool s_exit = false;

void exitHandler(int /*sig_num*/) 
{
	s_exit = true;
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

	signal(SIGINT, exitHandler);

	printf("mi_version(): %d\n", mi_version());

	server_create(
		"./",
		getCmdOption(argv, argv + argc, "(-r)"),
		getCmdOption(argv, argv + argc, "(-u)"));

	while (server_tick() && !s_exit) {};

	server_destroy();

	return EXIT_SUCCESS;
}
