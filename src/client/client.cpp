#include "capi.h"
#include "client.h"

#include <core/assert.h>
#include <core/file.h>
#include <core/upgradablesharedmutex.h>
#include <server/rpc.h>

#include <array>
#include <iostream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <system_error>

namespace client
{

static TaskCreateInfo<void> gRpcTask, gTickTask, gDrawTask;
static UpgradableSharedMutex gClientApplicationMutex;
static std::shared_ptr<Client> gClientApplication;
enum TaskState : uint8_t
{
	kTaskStateNone = 0,
	kTaskStateRunning = 1,
	kTaskStateShuttingDown = 2,
	kTaskStateDone = 3
};
static std::atomic<TaskState> gRpcTaskState = kTaskStateNone;
static std::atomic<TaskState> gTickTaskState = kTaskStateNone;
static std::atomic<TaskState> gDrawTaskState = kTaskStateNone;

static void Rpc(zmq::socket_t& socket, zmq::active_poller_t& poller)
{
	ZoneScopedN("client::Rpc");

	using namespace std::literals;
	using namespace zpp::bits::literals;

	if (gRpcTaskState == kTaskStateShuttingDown)
	{
		gRpcTaskState = kTaskStateDone;
		gRpcTaskState.notify_one();
		return;
	}

	std::shared_lock lock{gClientApplicationMutex};

	static constexpr unsigned kBufferSize = 64;
	std::array<std::byte, kBufferSize> responseData;
	std::array<std::byte, kBufferSize> requestData;

	zpp::bits::in inStream{responseData};
	zpp::bits::out outStream{requestData};

	server::RpcSay::client client{inStream, outStream};

	if (auto result = client.request<"Say"_sha256_int>("hello"s); failure(result))
		std::cerr << "client.request() returned error code: "
					<< std::make_error_code(result).message() << '\n';

	if (auto sendResult = socket.send(zmq::buffer(outStream.data().data(), outStream.position()), zmq::send_flags::none); !sendResult)
		std::cerr << "socket.send() failed" << '\n';

	for (bool responseFailure = true; responseFailure;)
	{
		if (auto socketCount = poller.wait(2ms); socketCount)
		{
			//std::cout << "got " << socketCount << " sockets hit" << std::endl;
			if (auto recvResult = socket.recv(zmq::buffer(responseData), zmq::recv_flags::dontwait); recvResult)
			{
				auto responseResult = client.response<"Say"_sha256_int>();
				responseFailure = failure(responseResult);
				if (responseFailure)
				{
					std::cerr << "client.response() returned error code: "
								<< std::make_error_code(responseResult.error()).message() << '\n';
					return;
				}
				
				//std::cout << "Say(\"hello\") returned: " << responseResult.value() << std::endl;
			}
			else
			{
				std::cerr << "socket.recv() failed" << '\n';
				return;
			}
		}

		// IMPORTANT: check for if we are shutting down. If we don't, we'll be stuck in an infinite loop since we are never releasing gClientApplicationMutex.
		if (gRpcTaskState == kTaskStateShuttingDown)
		{
			gRpcTaskState = kTaskStateDone;
			gRpcTaskState.notify_one();
			return;
		}
	}

	auto rpcTask = CreateTask(Rpc, socket, poller);
	AddDependency(gRpcTask.handle, rpcTask.handle, true);
	gRpcTask = rpcTask;
}

static void Tick()
{
	ZoneScopedN("client::Tick");

	if (gTickTaskState == kTaskStateShuttingDown)
	{
		gTickTaskState = kTaskStateDone;
		gTickTaskState.notify_one();
		return;
	}

	std::shared_lock lock{gClientApplicationMutex};

	gClientApplication->Tick();

	auto tickTask = CreateTask(Tick);
	AddDependency(gTickTask.handle, tickTask.handle, true);
	gTickTask = tickTask;
}

static void Draw()
{
	ZoneScopedN("client::Draw");

	if (gDrawTaskState == kTaskStateShuttingDown)
	{
		gDrawTaskState = kTaskStateDone;
		gDrawTaskState.notify_one();
		return;
	}

	std::shared_lock lock{gClientApplicationMutex};

	gClientApplication->Draw();

	auto drawTask = CreateTask(Draw);
	AddDependency(gDrawTask.handle, drawTask.handle, true);
	gDrawTask = drawTask;
}

} // namespace client

Client::~Client() noexcept(false)
{
	ZoneScopedN("Client::~Client");

	myPoller.remove(mySocket);
	mySocket.close();
	myContext.shutdown();
	myContext.close();

	std::cout << "Client shutting down, goodbye." << '\n';
}

void Client::OnEvent()
{
	ZoneScopedN("Client::tick");

	RhiApplication::OnEvent();
}

Client::Client(std::string_view name, Environment&& env, CreateWindowFunc createWindowFunc)
: RHIApplication(
	std::forward<std::string_view>(name),
	std::forward<Environment>(env),
	createWindowFunc)
, myContext(1)
, mySocket(myContext, zmq::socket_type::req)
{
	// auto toString = [](zmq::event_flags ef) -> std::string {
	// 	std::string result;
	// 	if (zmq::detail::enum_bit_and(ef, zmq::event_flags::pollin) != zmq::event_flags::none)
	// 		result += "pollin ";
	// 	if (zmq::detail::enum_bit_and(ef, zmq::event_flags::pollout) != zmq::event_flags::none)
	// 		result += "pollout ";
	// 	if (zmq::detail::enum_bit_and(ef, zmq::event_flags::pollerr) != zmq::event_flags::none)
	// 		result += "pollerr ";
	// 	if (zmq::detail::enum_bit_and(ef, zmq::event_flags::pollpri) != zmq::event_flags::none)
	// 		result += "pollpri ";
	// 	return result;
	// };

	mySocket.set(zmq::sockopt::linger, 0);
	mySocket.connect("tcp://localhost:5555");
	myPoller.add(mySocket, zmq::event_flags::pollin|zmq::event_flags::pollout, [/*&toString*/](zmq::event_flags /*evf*/) {
		//std::cout << "socket flags: " << toString(ef) << std::endl;
	});

	using namespace client;

	gRpcTask = CreateTask(Rpc, mySocket, myPoller);
	gRpcTaskState = kTaskStateRunning;
	gTickTask = CreateTask(client::Tick);
	gTickTaskState = kTaskStateRunning;
	gDrawTask = CreateTask(client::Draw);
	gDrawTaskState = kTaskStateRunning;

	std::array<TaskHandle, 2> handles{gRpcTask.handle, gUpdateTask.handle};
	GetExecutor().Submit(handles);

	// initial OnEvent call required to initialize data structures in imgui (and potentially others)
	// since RHIApplication draw thread/tasks can launch before next OnEvent is called from main
	OnEvent();
}

bool OnEventClient()
{	
	using namespace client;

	std::shared_lock lock{gClientApplicationMutex};

	ASSERT(gClientApplication);

	gClientApplication->OnEvent();

	return !gClientApplication->IsExitRequested();
}

void CreateClient(CreateWindowFunc createWindowFunc, const PathConfig* paths)
{
	using namespace client;
	using namespace file;

	ASSERT(paths != nullptr);

	auto root = GetCanonicalPath(nullptr, "./");

	auto resourcePath = GetCanonicalPath(paths->resourcePath, (root.value() / "resources").string().c_str());
	auto userPath = GetCanonicalPath(paths->userProfilePath, (root.value() / ".speedo").string().c_str(), true);

	if (!resourcePath || !userPath)
	{
		std::cerr << "Failed to get resource or user path" << '\n';
		return;
	}

	std::unique_lock lock{gClientApplicationMutex};

	gClientApplication = Application::Create<Client>(
		"client",
		Environment{{
			{"RootPath", root.value()},
			{"ResourcePath", resourcePath.value()},
			{"UserProfilePath", userPath.value()}
		}},
		createWindowFunc);

	ASSERT(gClientApplication);
}

void DestroyClient()
{
	using namespace client;

	gRpcTaskState = kTaskStateShuttingDown;
	gTickTaskState = kTaskStateShuttingDown;
	gDrawTaskState = kTaskStateShuttingDown;

	gRpcTaskState.wait(kTaskStateShuttingDown);
	gTickTaskState.wait(kTaskStateShuttingDown);
	gDrawTaskState.wait(kTaskStateShuttingDown);

	std::unique_lock lock{gClientApplicationMutex};

	ASSERT(gClientApplication);
	ASSERT(gClientApplication.use_count() == 1);
	
	gClientApplication.reset();
}
