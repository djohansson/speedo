#include "capi.h"

#include <core/application.h>
#include <core/rpc.h>

#define STB_SPRINTF_IMPLEMENTATION
#include <stb_sprintf.h>

#include <cassert>
#include <chrono>
#include <format>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace server
{

using namespace std::chrono_literals;
using namespace core;

class ServerApplication : public Application
{	
public:
	~ServerApplication() = default;

	bool tick() override
	{
		std::array<std::byte, 64> requestData{};
		std::array<std::byte, 64> responseData{};

		zpp::bits::in in{requestData};
		zpp::bits::out out{responseData};

		rpc::server server{in, out};

		if (auto recvResult = mySocket.recv(zmq::buffer(requestData), zmq::recv_flags::none))
		{
			std::cout << "received " << recvResult.value().size << " bytes." << std::endl;
			
			if (auto result = server.serve(); failure(result))
			{
				std::cout << "server.serve() returned error code: " << std::make_error_code(result).message() << std::endl;
			
				return false;
			}

			mySocket.send(zmq::buffer(out.data().data(), out.position()), zmq::send_flags::none);
		}

		// simulate work
		std::this_thread::sleep_for(100ms);

		return true;
	}

protected:
	ServerApplication(std::string_view name, Environment&& env)
	: Application(std::forward<std::string_view>(name), std::forward<Environment>(env))
	, myContext(1)
	, mySocket(myContext, zmq::socket_type::rep)
	{
		constexpr uint16_t cx_port = 5555;
		std::string serverAddress = std::format("tcp://*:{}", cx_port);

		mySocket.bind(serverAddress);
			
		std::cout << "Server listening on " << serverAddress << std::endl;
	}

private:
	zmq::context_t myContext;
	zmq::socket_t mySocket;
};

static std::shared_ptr<ServerApplication> s_application{};

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

} // namespace server

void server_create(
	const char* rootPath,
	const char* resourcePath,
	const char* userProfilePath)
{
	using namespace server;

	s_application = Application::create<server::ServerApplication>(
		"server",
		Environment{
			getCanonicalPath(rootPath, "./"),
			getCanonicalPath(resourcePath, "./resources/"),
			getCanonicalPath(userProfilePath, "./.profile/", true)
		});

	assert(s_application);
}

bool server_tick()
{
	using namespace server;

	assert(s_application);

	return s_application->tick();
}

void server_destroy()
{
	using namespace server;

	assert(s_application);
	
	s_application.reset();
}

const char* server_getAppName(void)
{
	using namespace server;

	assert(s_application);

	return s_application->name().data();
}
