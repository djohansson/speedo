#include "capi.h"
#include "server.h"
#include "rpc.h"

#include <core/application.h>
#include <core/assert.h>
#include <core/file.h>

#include <array>
#include <iostream>
#include <memory>
#include <system_error>

namespace server
{

static TaskCreateInfo<void> gRpcTask{};
static UpgradableSharedMutex gServerApplicationMutex;
static std::shared_ptr<Server> gServerApplication;
enum TaskState : uint8_t
{
	kTaskStateNone = 0,
	kTaskStateRunning = 1,
	kTaskStateShuttingDown = 2,
	kTaskStateDone = 3
};
static std::atomic<TaskState> gRpcTaskState = kTaskStateNone;

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

	if (gRpcTaskState == kTaskStateShuttingDown)
	{
		gRpcTaskState = kTaskStateDone;
		gRpcTaskState.notify_one();
		return;
	}

	std::shared_lock lock{gServerApplicationMutex};

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

	auto rpcTask = Task::CreateTask(Rpc, socket, poller);
	Task::AddDependency(gRpcTask.first, rpcTask.first);
	gRpcTask = rpcTask;
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
	gRpcTaskState = kTaskStateRunning;
	Executor().Submit({&gRpcTask.first, 1});
}

void CreateServer(const PathConfig* paths)
{
	using namespace server;
	using namespace file;

	ASSERT(paths != nullptr);

	auto root = GetCanonicalPath(nullptr, "./");

	if (!root)
	{
		std::cerr << "Failed to get root path" << '\n';
		return;
	}

	auto resourcePath = GetCanonicalPath(paths->resourcePath, (root.value() / "resources").string().c_str());
	auto userPath = GetCanonicalPath(paths->userProfilePath, (root.value() / ".speedo").string().c_str(), true);

	if (!resourcePath || !userPath)
	{
		std::cerr << "Failed to get resource or user path" << '\n';
		return;
	}

	std::unique_lock lock{gServerApplicationMutex};

	gServerApplication = Application::Create<Server>(
		"server",
		Environment{{
			{"RootPath", root.value()},
			{"ResourcePath", resourcePath.value()},
			{"UserProfilePath", userPath.value()}
		}});

	ASSERT(gServerApplication);
}

void DestroyServer()
{
	using namespace server;

	gRpcTaskState = kTaskStateShuttingDown;
	gRpcTaskState.wait(kTaskStateShuttingDown);

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
