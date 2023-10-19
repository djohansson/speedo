#include "rpc.h"

namespace core
{

using namespace std::literals;

std::string say(std::string s)
{
	if (s == "hello"s)
		return "world";
	
	return "nothing";
}

} // namespace core
