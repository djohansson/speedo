#include "capi.h"
#include "rpc.h"

#include <core/application.h>
#include <core/file.h>

#include <zmq.hpp>
#include <zmq_addon.hpp>

#include <array>
#include <cassert>
#include <iostream>
#include <memory>
#include <string_view>
#include <system_error>

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

static std::pair<TaskHandle, Future<void>> g_rpcTask{};

void rpc(TaskExecutor& executor, zmq::socket_t& socket, zmq::active_poller_t& poller)
{
	ZoneScopedN("server::rpc");

	if (auto app = Application::instance().lock(); !app || app->exitRequested())
		return;

	try
	{
		std::array<std::byte, 64> requestData;
		std::array<std::byte, 64> responseData;

		zpp::bits::in in{requestData};
		zpp::bits::out out{responseData};

		server::rpc_say::server server{in, out};

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

	// todo: move to own function?
	{
		auto rpcPair = executor.createTask(rpc, executor, socket, poller);
		auto& [rpcTask, rpcFuture] = rpcPair;
		executor.addDependency(g_rpcTask.first, rpcTask, true);
		g_rpcTask = rpcPair;
	}
}

class Server : public Application
{	
public:
	~Server()
	{
		ZoneScopedN("Server::~Server");

		g_rpcTask.second.wait();
		g_rpcTask = {};

		myPoller.remove(mySocket);
		mySocket.close();
		myContext.shutdown();
		myContext.close();

		std::cout << "Server shutting down, goodbye." << std::endl;
	}

	void tick() override
	{
		std::cout << "Press q to quit: ";
		char input;
		std::cin >> input;
		if (input == 'q')
			requestExit();
	}

protected:
	explicit Server() = default;
	Server(std::string_view name, Environment&& env)
	: Application(std::forward<std::string_view>(name), std::forward<Environment>(env))
	, myContext(1)
	, mySocket(myContext, zmq::socket_type::rep)
	{
		constexpr std::string_view cx_serverAddress = "tcp://*:5555"sv;

		mySocket.set(zmq::sockopt::linger, 0);
		mySocket.bind(cx_serverAddress.data());
		myPoller.add(mySocket, zmq::event_flags::pollin|zmq::event_flags::pollout, [/*&toString*/](zmq::event_flags ef) {
			//std::cout << "socket flags: " << toString(ef) << std::endl;
		});
			
		std::cout << "Server listening on " << cx_serverAddress << std::endl;

		auto& executor = internalExecutor();

		auto rpcPair = executor.createTask(rpc, executor, mySocket, myPoller);
		auto& [rpcTask, rpcFuture] = rpcPair;
		g_rpcTask = rpcPair;
		executor.submit(rpcTask);
	}

private:
	zmq::context_t myContext;
	zmq::socket_t mySocket;
	zmq::active_poller_t myPoller;
};

static std::shared_ptr<Server> s_application{};

} // namespace server

void server_create(const PathConfig* paths)
{
	using namespace server;
	using namespace file;

	assert(paths != nullptr);

	auto root = getCanonicalPath(nullptr, "./");

	s_application = Application::create<Server>(
		"server",
		Environment{{
			{"RootPath", root},
			{"ResourcePath", getCanonicalPath(paths->resourcePath, (root / "resources").string().c_str())},
			{"UserProfilePath", getCanonicalPath(paths->userProfilePath, (root / ".speedo").string().c_str(), true)}
		}});
	assert(s_application);
}

bool server_tick()
{
	using namespace server;

	assert(s_application);

	s_application->tick();

	return !s_application->exitRequested();
}

void server_destroy()
{
	using namespace server;

	assert(s_application);
	assert(s_application.use_count() == 1);
	
	s_application.reset();
}

const char* server_getAppName(void)
{
	using namespace server;

	assert(s_application);

	return s_application->name().data();
}
