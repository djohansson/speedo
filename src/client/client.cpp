#include "capi.h"

#include <core/rpc.h>
#include <rhi/rhiapplication.h>

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

	bool tick() override
	{
		FrameMark;
		
		ZoneScopedN("Client::tick");

		{
			ZoneScopedN("Client::tick::rpc");

			std::array<std::byte, 64> responseData;
			std::array<std::byte, 64> requestData;

			zpp::bits::in in{responseData};
			zpp::bits::out out{requestData};

			core::rpc::client client{in, out};

			if (auto result = client.request<"say"_sha256_int>("hello"s); failure(result))
			{
				std::cout << "client.request() returned error code: " << std::make_error_code(result).message() << std::endl;
			
				return false;
			}

			mySocket.send(zmq::buffer(out.data().data(), out.position()), zmq::send_flags::none);
			
			if (auto recvResult = mySocket.recv(zmq::buffer(responseData), zmq::recv_flags::none))
			{
				if (auto result = client.response<"say"_sha256_int>(); failure(result))
				{
					std::cout << "client.response() returned error code: " << std::make_error_code(result.error()).message() << std::endl;
				
					return false;
				}
			}
		}

		return RhiApplication::tick();
	}

protected:
	Client(std::string_view name, Environment&& env)
	: RhiApplication(std::forward<std::string_view>(name), std::forward<Environment>(env))
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

void client_create(
	const WindowState* window,
	const char* rootPath,
	const char* resourcePath,
	const char* userProfilePath)
{
	using namespace client;

	assert(window != nullptr);
	assert(window->handle != nullptr);

	s_application = Application::create<Client>(
		"client",
		Environment{
			getCanonicalPath(rootPath, "./"),
			getCanonicalPath(resourcePath, "./resources/"),
			getCanonicalPath(userProfilePath, "./.profile/", true)
		});

	assert(s_application);

	s_application->createDevice(*window);
}

bool client_tick()
{
	using namespace client;

	assert(s_application);

	return s_application->tick();
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
