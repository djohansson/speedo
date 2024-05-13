#include "capi.h"
#include "client.h"

#include <core/file.h>
#include <core/upgradablesharedmutex.h>
#include <server/rpc.h>

#include <array>
#include <cassert>
#include <iostream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <system_error>

namespace client
{

using TaskPair = std::pair<TaskHandle, Future<void>>;
static TaskPair gUpdateTask, gDrawTask, gRpcTask;
static UpgradableSharedMutex gApplicationMutex;
static std::shared_ptr<Client> gApplication;

template <typename F>
static TaskPair continuation(F&& f, TaskHandle dependency)
{
	std::shared_lock lock{gApplicationMutex};

	if (gApplication->exitRequested())
		return {NullTaskHandle, Future<void>{}};

	auto taskPair = gApplication->executor().createTask(std::forward<F>(f));
	
	gApplication->executor().addDependency(dependency, taskPair.first, true);

	return taskPair;
}

static void rpc(zmq::socket_t& socket, zmq::active_poller_t& poller)
{
	ZoneScopedN("client::rpc");

	std::shared_lock lock{gApplicationMutex};

	using namespace std::literals;
	using namespace zpp::bits::literals;

	try
	{
		std::array<std::byte, 64> responseData;
		std::array<std::byte, 64> requestData;

		zpp::bits::in in{responseData};
		zpp::bits::out out{requestData};

		server::RpcSay::client client{in, out};

		if (auto result = client.request<"Say"_sha256_int>("hello"s); failure(result))
			std::cerr << "client.request() returned error code: " << std::make_error_code(result).message() << std::endl;
		
		if (auto sendResult = socket.send(zmq::buffer(out.data().data(), out.position()), zmq::send_flags::none); !sendResult)
			std::cerr << "socket.send() failed" << std::endl;

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
						std::cerr << "client.response() returned error code: " << std::make_error_code(responseResult.error()).message() << std::endl;
						return;
					}
					
					//std::cout << "Say(\"hello\") returned: " << responseResult.value() << std::endl;
				}
				else
				{
					std::cerr << "socket.recv() failed" << std::endl;
					return;
				}
			}

			if (gApplication->exitRequested()) // IMPORTANT: check for exitRequested() here. If we don't, we'll be stuck in an infinite loop since we are never releasing gApplicationMutex.
				return;
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

	gRpcTask = continuation([&socket, &poller]{ rpc(socket, poller); }, gRpcTask.first);
}

static void	draw();
static void	updateInput()
{
	ZoneScopedN("client::updateInput");

	std::shared_lock lock{gApplicationMutex};

	gApplication->updateInput();
	gDrawTask = continuation(draw, gUpdateTask.first);
}

static void	draw()
{
	ZoneScopedN("client::draw");

	std::shared_lock lock{gApplicationMutex};

	gApplication->draw();
	gUpdateTask = continuation(updateInput, gDrawTask.first);
}

// updateInput -> draw -> updateInput -> draw -> ... until app termination

} // namespace client

Client::~Client()
{
	ZoneScopedN("Client::~Client");

	myPoller.remove(mySocket);
	mySocket.close();
	myContext.shutdown();
	myContext.close();

	std::cout << "Client shutting down, goodbye." << std::endl;
}

void Client::tick()
{
	ZoneScopedN("Client::tick");

	RhiApplication::tick();
}

Client::Client(std::string_view name, Environment&& env, CreateWindowFunc createWindowFunc)
: RhiApplication(
	std::forward<std::string_view>(name),
	std::forward<Environment>(env),
	createWindowFunc)
, myContext(1)
, mySocket(myContext, zmq::socket_type::req)
{
	using namespace client;
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
	myPoller.add(mySocket, zmq::event_flags::pollin|zmq::event_flags::pollout, [/*&toString*/](zmq::event_flags ef) {
		//std::cout << "socket flags: " << toString(ef) << std::endl;
	});

	gRpcTask = executor().createTask(rpc, mySocket, myPoller);
	executor().submit(gRpcTask.first);
}

bool TickClient()
{	
	using namespace client;

	std::shared_lock lock{gApplicationMutex};

	assert(gApplication);

	gApplication->tick();

	return !gApplication->exitRequested();
}

void CreateClient(CreateWindowFunc createWindowFunc, const PathConfig* paths)
{
	using namespace client;
	using namespace file;

	assert(paths != nullptr);

	auto root = getCanonicalPath(nullptr, "./");

	std::unique_lock lock{gApplicationMutex};

	gApplication = Application::create<Client>(
		"client",
		Environment{{
			{"RootPath", root},
			{"ResourcePath", getCanonicalPath(paths->resourcePath, (root / "resources").string().c_str())},
			{"UserProfilePath", getCanonicalPath(paths->userProfilePath, (root / ".speedo").string().c_str(), true)}
		}},
		createWindowFunc);

	assert(gApplication);
	
	gUpdateTask = gApplication->executor().createTask(updateInput);
	gApplication->executor().submit(gUpdateTask.first);
}

void DestroyClient()
{
	using namespace client;

	std::unique_lock lock{gApplicationMutex};

	assert(gApplication);
	assert(gApplication.use_count() == 1);
	
	gApplication.reset();
}
