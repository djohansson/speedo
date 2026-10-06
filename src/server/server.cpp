#include <core/application.h>
#include <core/assert.h>
#include <core/path.h>
#include <core/concurrentaccess.h>

#include <server/capi.h>
#include <server/server.h>
#include <server/rpc/rpc.h>

#include <array>
#include <iostream>
#include <memory>

namespace server
{

using namespace core;
using namespace core::file;

static TaskCreateInfo<void> gRpcTask{};
static ConcurrentAccess<std::shared_ptr<Server>> gServerApplication;
enum TaskState : uint8_t
{
	kTaskStateNone = 0,
	kTaskStateRunning = 1,
	kTaskStateShuttingDown = 2,
	kTaskStateDone = 3
};
static std::atomic<TaskState> gRpcTaskState = kTaskStateNone;

// marks a task chain as finished (for whatever reason), releasing a pending wait in StopTask
static void SetTaskDone(std::atomic<TaskState>& state)
{
	state = kTaskStateDone;
	state.notify_one();
}

// requests a task chain to stop. returns true if it was running, i.e. if WaitTaskStopped needs to be called.
// (exchange rather than store, so a chain that already ended, e.g. on an error, isn't waited on forever)
[[nodiscard]] static bool RequestTaskStop(std::atomic<TaskState>& state)
{
	return state.exchange(kTaskStateShuttingDown) == kTaskStateRunning;
}

static void WaitTaskStopped(std::atomic<TaskState>& state)
{
	state.wait(kTaskStateShuttingDown);
}

std::string Say(const std::string& str)
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
		SetTaskDone(gRpcTaskState);
		return;
	}

	static constexpr unsigned kBufferSize = 64;
	std::array<std::byte, kBufferSize> requestData;
	std::array<std::byte, kBufferSize> responseData;

	zpp::bits::in inStream{requestData};
	zpp::bits::out outStream{responseData};

	server::RpcSay::server server{inStream, outStream};

	/*if (auto socketCount = */poller.wait(2ms);/*)
	{
		std::cout << "got " << socketCount << " sockets hit" << std::endl;
	}*/

	if (auto recvResult = /*inEvent.*/socket.recv(zmq::buffer(requestData), zmq::recv_flags::dontwait))
	{
		if (auto result = server.serve(); failure(result))
		{
			std::cerr << "server.serve() returned error code: "
						<< std::make_error_code(result).message() << '\n';
			SetTaskDone(gRpcTaskState);
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
			SetTaskDone(gRpcTaskState);
			return;
		}
	}

	auto rpcTask = CreateTask(Rpc, socket, poller);
	AddDependency(gRpcTask.handle, rpcTask.handle, true);
	gRpcTask = rpcTask;
}

Server::~Server()
{
	ZoneScopedN("Server::~Server");

	myPoller.remove(mySocket);
	mySocket.close();
	myContext.shutdown();
	myContext.close();

	std::cout << "Server shutting down, goodbye." << '\n';
}

Server::Server(std::string_view name, Environment&& env)
: Application(std::forward<std::string_view>(name), std::forward<Environment>(env))
, myContext(1)
, mySocket(myContext, zmq::socket_type::rep)
{
	using namespace std::literals;

	constexpr std::string_view kCxServerAddress = "tcp://*:5555"sv;

	mySocket.set(zmq::sockopt::linger, 0);
	mySocket.bind(kCxServerAddress.data());
	myPoller.add(mySocket, zmq::event_flags::pollin|zmq::event_flags::pollout, [/*&toString*/](zmq::event_flags /*evf*/) {
		//std::cout << "socket flags: " << toString(ef) << std::endl;
	});

	std::cout << "Server listening on " << kCxServerAddress << '\n';

	gRpcTask = CreateTask(Rpc, mySocket, myPoller);
	gRpcTaskState = kTaskStateRunning;
}

} // namespace server

void ServerCreate(const PathConfig* paths)
{
	using namespace server;

	ENSURE(paths != nullptr);

	auto root = GetCanonicalPath(nullptr, "./");
	auto resourcePath = GetCanonicalPath(paths->resourcePath, (root.value() / "resources").string().c_str());
	auto userPath = GetCanonicalPath(paths->userProfilePath, (root.value() / ".speedo").string().c_str(), true);

	ENSURE(root);
	ENSURE(resourcePath);
	ENSURE(userPath);

	auto& appPtrRef = gServerApplication.Write().Get();
	appPtrRef = CreateApplication<Server>(
		"server",
		Environment{{
			{"RootPath", root.value()},
			{"ResourcePath", resourcePath.value()},
			{"UserProfilePath", userPath.value()},
		},});

	appPtrRef->GetExecutor().Submit({&gRpcTask.handle, 1});
}

void ServerDestroy()
{
	using namespace server;

	if (RequestTaskStop(gRpcTaskState))
		WaitTaskStopped(gRpcTaskState);

	auto appPtrWriteScope = gServerApplication.Write();

	ENSURE(appPtrWriteScope.Get());
	ASSERT(appPtrWriteScope.Get().use_count() == 1);

	// finish any in-flight tasks while the application is still registered in gApplication
	appPtrWriteScope.Get()->GetExecutor().JoinAll();

	appPtrWriteScope.Get().reset();
}

bool ServerExitRequested()
{
	using namespace server;

	return gServerApplication.Read()->IsExitRequested();
}
