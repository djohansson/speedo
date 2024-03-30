#include "capi.h"

#include <rhi/rhiapplication.h>
#include <server/rpc.h>

#include <zmq.hpp>

#include <array>
#include <cassert>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>

namespace client
{

using namespace std::literals;
using namespace zpp::bits::literals;

class Client : public RhiApplication
{	
public:
	~Client()
	{
		ZoneScopedN("Client::~Client");

		mySocket.close();
		myContext.shutdown();
		myContext.close();

		std::cout << "Client shutting down, goodbye." << std::endl;
	}

	void tick() override
	{
		ZoneScopedN("Client::tick");

		RhiApplication::tick();
	}

protected:
	explicit Client() = default;
	Client(std::string_view name, Environment&& env, const WindowState& window)
	: RhiApplication(
		std::forward<std::string_view>(name),
		std::forward<Environment>(env),
		window)
	, myContext(1)
	, mySocket(myContext, zmq::socket_type::req)
	{
		mySocket.connect("tcp://localhost:5555");
	}

private:
	zmq::context_t myContext;
	zmq::socket_t mySocket;
};

static std::shared_ptr<Client> s_application{};

std::filesystem::path
getCanonicalPath(const char* pathStr, const char* defaultPathStr, bool createIfMissing = false)
{
	assert(defaultPathStr != nullptr);
	
	auto path = std::filesystem::path(pathStr ? pathStr : defaultPathStr);

	if (createIfMissing && !std::filesystem::exists(path))
		std::filesystem::create_directory(path);

	assert(std::filesystem::is_directory(path));

	return std::filesystem::canonical(path);
}

} // namespace client

void client_create(const WindowState* window, const PathConfig* paths)
{
	using namespace client;

	assert(window != nullptr);
	assert(window->handle != nullptr);
	assert(paths != nullptr);

	auto root = getCanonicalPath(nullptr, "./");

	s_application = Application::create<Client>(
		"client",
		Environment{{
			{"RootPath", root},
			{"ResourcePath", getCanonicalPath(paths->resourcePath, (root / "resources").string().c_str())},
			{"UserProfilePath", getCanonicalPath(paths->userProfilePath, (root / ".profile").string().c_str(), true)}
		}},
		*window);

	assert(s_application);
}

bool client_tick()
{
	using namespace client;

	assert(s_application);

	s_application->tick();

	return !s_application->exitRequested();
}

void client_resizeWindow(const WindowState* state)
{
	using namespace client;

	assert(state != nullptr);
	assert(s_application);
	
	s_application->onResizeWindow(*state);
}

void client_resizeFramebuffer(uint32_t width, uint32_t height)
{
	using namespace client;

	assert(s_application);
	
	s_application->onResizeFramebuffer(width, height);
}

void client_mouse(const MouseState* state)
{
	using namespace client;

	assert(state != nullptr);
	assert(s_application);
	
	s_application->onMouse(*state);
}

void client_keyboard(const KeyboardState* state)
{
	using namespace client;

	assert(state != nullptr);
	assert(s_application);
	
	s_application->onKeyboard(*state);
}

void client_destroy()
{
	using namespace client;

	assert(s_application);
	
	s_application.reset();
}

const char* client_getAppName(void)
{
	using namespace client;

	assert(s_application);

	return s_application->name().data();
}
