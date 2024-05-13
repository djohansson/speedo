#pragma once

#include <rhi/rhiapplication.h>

#include <string_view>

#include <zmq.hpp>
#include <zmq_addon.hpp>

class Client : public RhiApplication
{	
public:
	~Client() noexcept(false) override;

	void Tick() override;

protected:
	explicit Client() = default;
	Client(std::string_view name, Environment&& env, CreateWindowFunc createWindowFunc);

private:
	zmq::context_t myContext;
	zmq::socket_t mySocket;
	zmq::active_poller_t myPoller;
};
