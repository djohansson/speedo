#pragma once

#include <zpp_bits.h>

#include <string>

namespace server
{

std::string Say(std::string s);

using namespace zpp::bits::literals;
using RpcSay = zpp::bits::rpc<zpp::bits::bind<server::Say, "Say"_sha256_int>>;

}
