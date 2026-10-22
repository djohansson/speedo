#include <core/assert.h>
#include <core/capi.h>

#include <server/capi.h>

#include <stdio.h>
#include <stdlib.h>

#if defined(SPEEDO_USE_MIMALLOC)
#include <mimalloc.h>
#endif

int main(int argc, char* argv[], char* envp[])
{
#if defined(SPEEDO_USE_MIMALLOC)
	mi_version(); // if not called first thing in main(), malloc will not be redirected correctly on windows
#endif

	// the main loop polls IsInterrupted, so no wake up is needed
	InstallSignalHandlers(NULL);

	ENSURE(argv != NULL);
	ENSURE(envp != NULL);

	struct PathConfig paths = {NULL, NULL};
	if (!ParseArguments(argc, argv, "server", &paths))
		return EXIT_SUCCESS;

	ServerCreate(&paths);

	fprintf(stdout, "Press Ctrl-C to quit\n");

	while (!ServerExitRequested() && !IsInterrupted())
		SleepSeconds(0.1);

	ServerDestroy();

	return EXIT_SUCCESS;
}
