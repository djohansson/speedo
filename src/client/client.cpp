#include "capi.h"

#include <core/rpc.h>
#include <rhi/rhiapplication.h>

#define STB_SPRINTF_IMPLEMENTATION
#include <stb_sprintf.h>

#include <cassert>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

namespace client
{

using namespace std::literals;
using namespace core;

class ClientApplication : public RhiApplication
{	
public:
	~ClientApplication() = default;

	bool tick() override
	{
		std::array<std::byte, 64> responseData{};
		std::array<std::byte, 64> requestData{};

		zpp::bits::in in{responseData};
		zpp::bits::out out{requestData};

		rpc::client client{in, out};

		static const std::string cs_helloStr = "hello"s;

		if (auto result = client.request<"say"_sha256_int>(cs_helloStr); failure(result))
		{
			std::cout << "client.request() returned error code: " << std::make_error_code(result).message() << std::endl;
		
			return false;
		}
		else
		{
			std::cout << "client.request(): " << cs_helloStr << std::endl;
		}

		mySocket.send(zmq::buffer(out.data().data(), out.position()), zmq::send_flags::none);
		
		if (auto recvResult = mySocket.recv(zmq::buffer(responseData), zmq::recv_flags::none))
		{
			std::cout << "received " << recvResult.value().size << " bytes." << std::endl;
			
			auto result = client.response<"say"_sha256_int>(); 

			if (failure(result))
			{
				std::cout << "client.response() returned error code: " << std::make_error_code(result.error()).message() << std::endl;
			
				return false;
			}
			else
			{
				std::cout << "client.response(): " << result.value() << std::endl;
			}
		}

		return RhiApplication::tick();
	}

protected:
	ClientApplication(std::string_view name, Environment&& env)
	: RhiApplication(std::forward<std::string_view>(name), std::forward<Environment>(env))
	, myContext(1)
	, mySocket(myContext, zmq::socket_type::req)
	{
		constexpr uint16_t cx_port = 5555;
		std::string serverAddress = std::format("tcp://localhost:{}", cx_port);

		mySocket.connect(serverAddress);
	}

private:
	zmq::context_t myContext;
	zmq::socket_t mySocket;
};

static std::shared_ptr<ClientApplication> s_application{};

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

	s_application = Application::create<ClientApplication>(
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
