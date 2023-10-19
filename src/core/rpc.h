#pragma once

#include <zmq.hpp>

#include <zpp_bits.h>

#include <string>

namespace core
{

using namespace zpp::bits::literals;

std::string say(std::string s);

using rpc = zpp::bits::rpc<zpp::bits::bind<say, "say"_sha256_int>>;

} // namespace core
