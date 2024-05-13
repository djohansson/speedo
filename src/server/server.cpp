#include "capi.h"
#include "server.h"
#include "rpc.h"

#include <core/application.h>
#include <core/file.h>

#include <array>
#include <cassert>
#include <iostream>
#include <memory>
#include <system_error>

namespace server
{

using TaskPair = std::pair<TaskHandle, Future<void>>;
static TaskPair g_rpcTask{};
static UpgradableSharedMutex g_applicationMutex;
static std::shared_ptr<Server> g_application;

template <typename F>
static TaskPair continuation(F&& f, TaskHandle dependency)
{
	std::shared_lock lock{g_applicationMutex};

	if (g_application->exitRequested())
		return {NullTaskHandle, Future<void>{}};

	auto taskPair = g_application->executor().createTask(std::forward<F>(f));
	
	g_application->executor().addDependency(dependency, taskPair.first, true);

	return taskPair;
}

static std::string Say(std::string s)
{
	using namespace std::literals;

	if (s == "hello"s)
		return "world"s;
	
	return "nothing"s;
}

static void rpc(zmq::socket_t& socket, zmq::active_poller_t& poller)
{
	ZoneScopedN("server::rpc");

	using namespace std::literals::chrono_literals;
	using namespace zpp::bits::literals;

	std::shared_lock lock{g_applicationMutex};

	try
	{
		std::array<std::byte, 64> requestData;
		std::array<std::byte, 64> responseData;

		zpp::bits::in in{requestData};
		zpp::bits::out out{responseData};

		server::RpcSay::server server{in, out};

		if (auto socketCount = poller.wait(2ms))
		{
			//std::cout << "got " << socketCount << " sockets hit" << std::endl;
		}

		if (auto recvResult = /*inEvent.*/socket.recv(zmq::buffer(requestData), zmq::recv_flags::dontwait))
		{
			if (auto result = server.serve(); failure(result))
			{
				std::cerr << "server.serve() returned error code: " << std::make_error_code(result).message() << std::endl;
				return;
			}
			
			// if (!outPoller.wait(outEvent, timeout))
			// {
			// 	std::cout << "output timeout, try again" << std::endl;
			// 	return;
			// }

			if (auto sendResult = /*outEvent.*/socket.send(zmq::buffer(out.data().data(), out.position()), zmq::send_flags::none); !sendResult)
			{
				std::cerr << "socket.send() failed" << std::endl;
				return;
			}
		}
	}
	catch (zmq::error_t& error)
	{
		std::cerr << "zmq exception: " << error.what() << std::endl;
		return;
	}
	catch (...)
	{
		std::cerr << "unknown exception" << std::endl;
		return;
	}

	g_rpcTask = continuation([&socket, &poller]{ rpc(socket, poller); }, g_rpcTask.first);
}

} // namespace server

Server::~Server()
{
	ZoneScopedN("Server::~Server");

	myPoller.remove(mySocket);
	mySocket.close();
	myContext.shutdown();
	myContext.close();

	std::cout << "Server shutting down, goodbye." << std::endl;
}

void Server::tick()
{
	std::cout << "Press q to quit: ";
	char input;
	std::cin >> input;
	if (input == 'q')
		requestExit();

	Application::tick();
}

Server::Server(std::string_view name, Environment&& env)
: Application(std::forward<std::string_view>(name), std::forward<Environment>(env))
, myContext(1)
, mySocket(myContext, zmq::socket_type::rep)
{
	using namespace server;
	using namespace std::literals;

	constexpr std::string_view cx_serverAddress = "tcp://*:5555"sv;

	mySocket.set(zmq::sockopt::linger, 0);
	mySocket.bind(cx_serverAddress.data());
	myPoller.add(mySocket, zmq::event_flags::pollin|zmq::event_flags::pollout, [/*&toString*/](zmq::event_flags ef) {
		//std::cout << "socket flags: " << toString(ef) << std::endl;
	});
		
	std::cout << "Server listening on " << cx_serverAddress << std::endl;

	g_rpcTask = executor().createTask(rpc, mySocket, myPoller);
	executor().submit(g_rpcTask.first);
}

void CreateServer(const PathConfig* paths)
{
	using namespace server;
	using namespace file;

	assert(paths != nullptr);

	auto root = getCanonicalPath(nullptr, "./");

	std::unique_lock lock{g_applicationMutex};

	g_application = Application::create<Server>(
		"server",
		Environment{{
			{"RootPath", root},
			{"ResourcePath", getCanonicalPath(paths->resourcePath, (root / "resources").string().c_str())},
			{"UserProfilePath", getCanonicalPath(paths->userProfilePath, (root / ".speedo").string().c_str(), true)}
		}});

	assert(g_application);
}

void DestroyServer()
{
	using namespace server;

	std::unique_lock lock{g_applicationMutex};

	assert(g_application);
	assert(g_application.use_count() == 1);
	
	g_application.reset();
}

bool TickServer()
{
	using namespace server;

	std::shared_lock lock{g_applicationMutex};

	assert(g_application);

	g_application->tick();

	return !g_application->exitRequested();
}

const char* GetServerName(void)
{
	using namespace server;

	std::shared_lock lock{g_applicationMutex};

	assert(g_application);

	return g_application->name().data();
}
