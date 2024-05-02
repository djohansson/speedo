#include <catch2/catch_session.hpp>

#include <mimalloc.h>

int main(int argc, char* argv[])
{
	(void)mi_version();

	return Catch::Session().run(argc, argv);
}
