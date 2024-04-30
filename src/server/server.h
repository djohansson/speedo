#pragma once

#include <core/application.h>

#include <string_view>

#include <zmq.hpp>
#include <zmq_addon.hpp>

class Server : public Application
{	
public:
	~Server() override;
	
	void tick() override;

protected:
	explicit Server() = default;
	Server(std::string_view name, Environment&& env);

private:
	zmq::context_t myContext;
	zmq::socket_t mySocket;
	zmq::active_poller_t myPoller;
};
