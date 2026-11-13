#pragma once

#include <core/capi.h>
#include <core/inputstate.h>
#include <core/eventhandlers.h>
#include <client/windowedapplication.h>

#include <string_view>

#include <zmq.hpp>
#include <zmq_addon.hpp>

namespace client::inline SPEEDO_GRAPHICS_API_NAMESPACE
{

class Client final : public WindowedApplication, public core::KeyboardEventHandler, public core::MouseEventHandler
{	
public:
	Client(std::string_view name, core::Environment&& env);
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
	std::array<std::chrono::steady_clock::time_point, 2> myTimestamps;
};

// the copy's client (its window, its tasks), which the client's C api runs for the graphics api SPEEDO_GRAPHICS_API names
// (client/capi.cpp, compiled once): Create once the window system is, Main on the main thread until it returns false,
// Destroy last
void Create(const PathConfig* paths);
bool Main();
void Destroy();

} // namespace client::inline SPEEDO_GRAPHICS_API_NAMESPACE
