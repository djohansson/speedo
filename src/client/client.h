#pragma once

#include <core/inputstate.h>
#include <core/eventhandlers.h>
#include <gfx/windowedapplication.h>

#include <string_view>

#include <zmq.hpp>
#include <zmq_addon.hpp>

namespace client
{

class Client final : public gfx::WindowedApplication, public core::KeyboardEventHandler, public core::MouseEventHandler
{	
public:
	Client(std::string_view name, core::Environment&& env, CreateWindowFunc createWindowFunc);
	~Client() final;

	void OnKeyboard(const KeyboardEvent& keyboard) final;
	void OnMouse(const MouseEvent& mouse) final;
	bool Main() final;

	void Tick();

private:
	zmq::context_t myContext;
	zmq::socket_t mySocket;
	zmq::active_poller_t myPoller;

	mutable core::ConcurrentQueue<MouseEvent> myMouseQueue;
	mutable core::ConcurrentQueue<KeyboardEvent> myKeyboardQueue;
	core::InputState myInput{};
	std::array<std::chrono::high_resolution_clock::time_point, 2> myTimestamps;
};

} // namespace client
