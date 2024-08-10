#include <catch2/catch_session.hpp>

#include <mimalloc.h>

int main(int argc, char* argv[])
{
	mi_version(); // if not called first thing in main(), malloc will not be redirected correctly on windows
	
	return Catch::Session().run(argc, argv);
}
