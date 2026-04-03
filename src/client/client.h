#pragma once

#include <core/inputstate.h>
#include <core/eventhandlers.h>
#include <rhi/rhiapplication.h>

#include <string_view>

#include <zmq.hpp>
#include <zmq_addon.hpp>

class Client final : public RHIApplication, public KeyboardEventHandler, public MouseEventHandler
{	
public:
	explicit Client() = default;
	Client(std::string_view name, Environment&& env, CreateWindowFunc createWindowFunc);
	~Client() noexcept(false) final;

	void OnKeyboard(const KeyboardEvent& keyboard) final;
	void OnMouse(const MouseEvent& mouse) final;
	bool Main() final;

	void Tick();

private:
	zmq::context_t myContext;
	zmq::socket_t mySocket;
	zmq::active_poller_t myPoller;

	mutable ConcurrentQueue<MouseEvent> myMouseQueue;
	mutable ConcurrentQueue<KeyboardEvent> myKeyboardQueue;
	InputState myInput{};
	std::array<std::chrono::high_resolution_clock::time_point, 2> myTimestamps;
};
