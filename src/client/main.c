#include <core/assert.h>
#include <core/capi.h>

#include <client/capi.h>

#include <platform/capi.h>

#include <stdlib.h>

#if defined(SPEEDO_USE_MIMALLOC)
#include <mimalloc.h>
#endif

int main(int argc, char* argv[], char* envp[])
{
#if defined(SPEEDO_USE_MIMALLOC)
	mi_version(); // if not called first thing in main(), malloc will not be redirected correctly on windows
#endif

	// an interrupt (ctrl-c) wakes the main loop, which sleeps in PlatformWaitEvents() until the next window event
	InstallSignalHandlers(PlatformWakeMain);

	ENSURE(argv != NULL);
	ENSURE(envp != NULL);

	struct PathConfig paths = {NULL, NULL};
	if (!ParseArguments(argc, argv, "client", &paths))
		return EXIT_SUCCESS;

	// the window system first: the client makes its window
	PlatformInit();
	ClientCreate(&paths);
	// with a timeout: a wake up (for mainCalls) that something peeked at before the wait looked is no new message to it,
	// and the main thread slept on until the next input (a fullscreen switch waiting for its main call stalled, twice,
	// until then)
	while (PlatformWaitEvents(0.1) && ClientMain() && !IsInterrupted())
		;
	ClientDestroy();
	PlatformTerminate();

	return EXIT_SUCCESS;
}
