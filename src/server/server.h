#pragma once

#include <core/application.h>

#include <string_view>

#include <zmq.hpp>
#include <zmq_addon.hpp>

class Server final : public Application
{	
public:
	explicit Server() = default;
	Server(std::string_view name, Environment&& env);
	~Server() noexcept(false) final;

private:
	zmq::context_t myContext;
	zmq::socket_t mySocket;
	zmq::active_poller_t myPoller;
};
