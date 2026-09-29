#pragma once

#include <core/application.h>

#include <string_view>

#include <zmq.hpp>
#include <zmq_addon.hpp>

namespace server
{

class Server final : public core::Application
{	
public:
	Server(std::string_view name, core::Environment&& env);
	~Server() final;

private:
	zmq::context_t myContext;
	zmq::socket_t mySocket;
	zmq::active_poller_t myPoller;
};

} // namespace server
