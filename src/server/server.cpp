#include "capi.h"
#include "rpc.h"

#include <core/application.h>

#include <zmq.hpp>

#include <array>
#include <cassert>
#include <iostream>
#include <memory>
#include <string_view>
#include <system_error>
#include <thread>

namespace server
{

using namespace std::literals;
using namespace std::literals::chrono_literals;
using namespace zpp::bits::literals;

std::string say(std::string s)
{
	if (s == "hello"s)
		return "world"s;
	
	return "nothing"s;
}

class Server : public Application
{	
public:
	~Server()
	{
		ZoneScopedN("Server::~Server");

		mySocket.close();
		myContext.shutdown();
		myContext.close();

		std::cout << "Server shutting down, goodbye." << std::endl;
	}

	bool tick() override
	{
		auto frameStart = std::chrono::steady_clock::now();

		FrameMark;
		
		ZoneScopedN("Server::tick");

		{
			ZoneScopedN("Server::tick::rpc");

			zpp::bits::in in{myRequestData};
			zpp::bits::out out{myResponseData};

			server::rpc::server server{in, out};

			if (auto recvResult = mySocket.recv(zmq::buffer(myRequestData), zmq::recv_flags::dontwait))
			{
				if (auto result = server.serve(); failure(result))
				{
					std::cerr << "server.serve() returned error code: " << std::make_error_code(result).message() << std::endl;
				
					return false;
				}

				if (auto sendResult = mySocket.send(zmq::buffer(out.data().data(), out.position()), zmq::send_flags::none); !sendResult)
				{
					std::cerr << "mySocket.send() failed" << std::endl;
				
					return false;
				}
			}
		}

		auto timeElapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - frameStart);

		if (timeElapsed < 16666us)
			std::this_thread::sleep_for(16666us - timeElapsed);

		std::cout << "Server::tick() time (us):" << timeElapsed.count() << std::endl;

		return true;
	}

protected:
	Server(std::string_view name, Environment&& env)
	: Application(std::forward<std::string_view>(name), std::forward<Environment>(env))
	, myContext(1)
	, mySocket(myContext, zmq::socket_type::rep)
	{
		constexpr std::string_view cx_serverAddress = "tcp://*:5555"sv;

		mySocket.bind(cx_serverAddress.data());
			
		std::cout << "Server listening on " << cx_serverAddress << std::endl;
	}

private:
	zmq::context_t myContext;
	zmq::socket_t mySocket;
	std::array<std::byte, 64> myRequestData;
	std::array<std::byte, 64> myResponseData;
};

static std::shared_ptr<Server> s_application{};

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

void server_create(const PathConfig* paths)
{
	using namespace server;

	assert(paths != nullptr);

	auto root = getCanonicalPath(nullptr, "./");

	s_application = Application::create<Server>(
		"server",
		Environment{{
			{"RootPath", root},
			{"ResourcePath", getCanonicalPath(paths->resourcePath, (root / "resources").string().c_str())},
			{"UserProfilePath", getCanonicalPath(paths->userProfilePath, (root / ".profile").string().c_str(), true)}
		}});
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
