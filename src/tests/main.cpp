#include <catch2/catch_session.hpp>

#if defined(SPEEDO_USE_MIMALLOC)
#include <mimalloc.h>
#endif

int main(int argc, char* argv[])
{
#if defined(SPEEDO_USE_MIMALLOC)
	mi_version(); // if not called first thing in main(), malloc will not be redirected correctly on windows
#endif
	
	return Catch::Session().run(argc, argv);
}
