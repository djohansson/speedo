#pragma once

#include <core/inputstate.h>
#include <core/eventhandlers.h>
#include <rhi/rhiapplication.h>

#include <string_view>

#include <zmq.hpp>
#include <zmq_addon.hpp>

class Client : public RHIApplication, public KeyboardEventHandler, public MouseEventHandler
{	
public:
	~Client() noexcept(false) override;

	void OnKeyboard(const KeyboardEvent& keyboard) override;
	void OnMouse(const MouseEvent& mouse) override;
	bool Main() override;

	void Tick();

protected:
	explicit Client() = default;
	Client(std::string_view name, Environment&& env, CreateWindowFunc createWindowFunc);

private:
	zmq::context_t myContext;
	zmq::socket_t mySocket;
	zmq::active_poller_t myPoller;

	ConcurrentQueue<MouseEvent> myMouseQueue;
	ConcurrentQueue<KeyboardEvent> myKeyboardQueue;
	InputState myInput{};
};
