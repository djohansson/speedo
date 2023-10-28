#pragma once

#include <zpp_bits.h>

#include <string>

namespace server
{

std::string say(std::string s);

using namespace zpp::bits::literals;
using rpc = zpp::bits::rpc<zpp::bits::bind<server::say, "say"_sha256_int>>;

}
