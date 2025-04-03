#pragma once

#include <zpp_bits.h>

#include <string>

namespace server
{

[[nodiscard]] std::string Say(const std::string& str);

using namespace zpp::bits::literals;
using RpcSay = zpp::bits::rpc<zpp::bits::bind<server::Say, "Say"_sha256_int>>;

}
