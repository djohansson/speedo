#include "capi.h"
#include "server.h"
#include "rpc.h"

#include <core/application.h>
#include <core/file.h>

#include <array>
#include "assert.h"//NOLINT(modernize-deprecated-headers)
#include <iostream>
#include <memory>
#include <system_error>

namespace server
{

static TaskCreateInfo<void> gRpcTask{};
static UpgradableSharedMutex gServerApplicationMutex;
static std::shared_ptr<Server> gServerApplication;

template <
	typename... Params,
	typename... Args,
	typename F,
	typename C = std::decay_t<F>,
	typename ArgsTuple = std::tuple<Args...>,
	typename ParamsTuple = std::tuple<Params...>,
	typename R = std_extra::apply_result_t<C, std_extra::tuple_cat_t<ArgsTuple, ParamsTuple>>>
requires std_extra::applicable<C, std_extra::tuple_cat_t<ArgsTuple, ParamsTuple>>
static TaskCreateInfo<R> Continuation(F&& callable, TaskHandle dependency)
{
	std::shared_lock lock{gServerApplicationMutex};

	if (gServerApplication->IsExitRequested())
		return {InvalidTaskHandle, Future<void>{}};

	auto taskPair = Task::CreateTask(std::forward<F>(callable));
	
	Task::AddDependency(dependency, taskPair.first, true);

	return taskPair;
}

static std::string Say(const std::string& str)
{
	using namespace std::literals;

	if (str == "hello"s)
		return "world"s;
	
	return "nothing"s;
}

static void Rpc(zmq::socket_t& socket, zmq::active_poller_t& poller)
{
	ZoneScopedN("server::Rpc");

	using namespace std::literals::chrono_literals;
	using namespace zpp::bits::literals;

	std::shared_lock lock{gServerApplicationMutex};

	try
	{
		static constexpr unsigned kBufferSize = 64;
		std::array<std::byte, kBufferSize> requestData;
		std::array<std::byte, kBufferSize> responseData;

		zpp::bits::in inStream{requestData};
		zpp::bits::out outStream{responseData};

		server::RpcSay::server server{inStream, outStream};

		if (auto socketCount = poller.wait(2ms))
		{
			//std::cout << "got " << socketCount << " sockets hit" << std::endl;
		}

		if (auto recvResult = /*inEvent.*/socket.recv(zmq::buffer(requestData), zmq::recv_flags::dontwait))
		{
			if (auto result = server.serve(); failure(result))
			{
				std::cerr << "server.serve() returned error code: "
						  << std::make_error_code(result).message() << '\n';
				return;
			}
			
			// if (!outPoller.wait(outEvent, timeout))
			// {
			// 	std::cout << "output timeout, try again" << std::endl;
			// 	return;
			// }

			if (auto sendResult = /*outEvent.*/socket.send(zmq::buffer(outStream.data().data(), outStream.position()), zmq::send_flags::none); !sendResult)
			{
				std::cerr << "socket.send() failed" << '\n';
				return;
			}
		}
	}
	catch (zmq::error_t& error)
	{
		std::cerr << "zmq exception: " << error.what() << '\n';
		return;
	}
	catch (...)
	{
		std::cerr << "unknown exception" << '\n';
		return;
	}

	gRpcTask = Continuation([&socket, &poller]{ Rpc(socket, poller); }, gRpcTask.first);
}

} // namespace server

Server::~Server() noexcept(false)
{
	ZoneScopedN("Server::~Server");

	myPoller.remove(mySocket);
	mySocket.close();
	myContext.shutdown();
	myContext.close();

	std::cout << "Server shutting down, goodbye." << '\n';
}

void Server::Tick()
{
	std::cout << "Press q to quit: ";
	char input;
	std::cin >> input;
	if (input == 'q')
		RequestExit();

	Application::Tick();
}

Server::Server(std::string_view name, Environment&& env)
: Application(std::forward<std::string_view>(name), std::forward<Environment>(env))
, myContext(1)
, mySocket(myContext, zmq::socket_type::rep)
{
	using namespace server;
	using namespace std::literals;

	constexpr std::string_view kCxServerAddress = "tcp://*:5555"sv;

	mySocket.set(zmq::sockopt::linger, 0);
	mySocket.bind(kCxServerAddress.data());
	myPoller.add(mySocket, zmq::event_flags::pollin|zmq::event_flags::pollout, [/*&toString*/](zmq::event_flags /*evf*/) {
		//std::cout << "socket flags: " << toString(ef) << std::endl;
	});

	std::cout << "Server listening on " << kCxServerAddress << '\n';

	gRpcTask = Task::CreateTask(Rpc, mySocket, myPoller);
	Executor().Submit(gRpcTask.first);
}

void CreateServer(const PathConfig* paths)
{
	using namespace server;
	using namespace file;

	ASSERT(paths != nullptr);

	auto root = GetCanonicalPath(nullptr, "./");

	std::unique_lock lock{gServerApplicationMutex};

	gServerApplication = Application::Create<Server>(
		"server",
		Environment{{
			{"RootPath", root},
			{"ResourcePath", GetCanonicalPath(paths->resourcePath, (root / "resources").string().c_str())},
			{"UserProfilePath", GetCanonicalPath(paths->userProfilePath, (root / ".speedo").string().c_str(), true)}
		}});

	ASSERT(gServerApplication);
}

void DestroyServer()
{
	using namespace server;

	std::unique_lock lock{gServerApplicationMutex};

	ASSERT(gServerApplication);
	ASSERT(gServerApplication.use_count() == 1);
	
	gServerApplication.reset();
}

bool TickServer()
{
	using namespace server;

	std::shared_lock lock{gServerApplicationMutex};

	ASSERT(gServerApplication);

	gServerApplication->Tick();

	return !gServerApplication->IsExitRequested();
}
