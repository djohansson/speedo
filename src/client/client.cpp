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

static TaskCreateInfo<void> gUpdateTask, gDrawTask, gRpcTask;
static UpgradableSharedMutex gClientApplicationMutex;
static std::shared_ptr<Client> gClientApplication;

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
	std::shared_lock lock{gClientApplicationMutex};

	if (gClientApplication->IsExitRequested())
		return {NullTaskHandle, Future<void>{}};

	auto taskPair = gClientApplication->Executor().CreateTask(std::forward<F>(callable));
	
	gClientApplication->Executor().AddDependency(dependency, taskPair.first, true);

	return taskPair;
}

static void Rpc(zmq::socket_t& socket, zmq::active_poller_t& poller)
{
	ZoneScopedN("client::rpc");

	std::shared_lock lock{gClientApplicationMutex};

	using namespace std::literals;
	using namespace zpp::bits::literals;

	try
	{
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

			if (gClientApplication->IsExitRequested()) // IMPORTANT: check for IsExitRequested() here. If we don't, we'll be stuck in an infinite loop since we are never releasing gClientApplicationMutex.
				return;
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

	gRpcTask = Continuation([&socket, &poller] { Rpc(socket, poller); }, gRpcTask.first);
}

static void Draw();
static void UpdateInput()
{
	ZoneScopedN("client::updateInput");

	std::shared_lock lock{gClientApplicationMutex};

	gClientApplication->UpdateInput();
	gDrawTask = Continuation(Draw, gUpdateTask.first);
}

static void Draw()
{
	ZoneScopedN("client::draw");

	std::shared_lock lock{gClientApplicationMutex};

	gClientApplication->Draw();
	gUpdateTask = Continuation(UpdateInput, gDrawTask.first);
}

// updateInput -> draw -> updateInput -> draw -> ... until app termination

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

void Client::Tick()
{
	ZoneScopedN("Client::tick");

	RhiApplication::Tick();
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
	myPoller.add(mySocket, zmq::event_flags::pollin|zmq::event_flags::pollout, [/*&toString*/](zmq::event_flags /*evf*/) {
		//std::cout << "socket flags: " << toString(ef) << std::endl;
	});

	gRpcTask = Executor().CreateTask(Rpc, mySocket, myPoller);
	Executor().Submit(gRpcTask.first);
}

bool TickClient()
{	
	using namespace client;

	std::shared_lock lock{gClientApplicationMutex};

	ASSERT(gClientApplication);

	gClientApplication->Tick();

	return !gClientApplication->IsExitRequested();
}

void CreateClient(CreateWindowFunc createWindowFunc, const PathConfig* paths)
{
	using namespace client;
	using namespace file;

	ASSERT(paths != nullptr);

	auto root = getCanonicalPath(nullptr, "./");

	std::unique_lock lock{gClientApplicationMutex};

	gClientApplication = Application::Create<Client>(
		"client",
		Environment{{
			{"RootPath", root},
			{"ResourcePath", getCanonicalPath(paths->resourcePath, (root / "resources").string().c_str())},
			{"UserProfilePath", getCanonicalPath(paths->userProfilePath, (root / ".speedo").string().c_str(), true)}
		}},
		createWindowFunc);

	ASSERT(gClientApplication);

	gUpdateTask = gClientApplication->Executor().CreateTask(UpdateInput);
	gClientApplication->Executor().Submit(gUpdateTask.first);
}

void DestroyClient()
{
	using namespace client;

	std::unique_lock lock{gClientApplicationMutex};

	ASSERT(gClientApplication);
	ASSERT(gClientApplication.use_count() == 1);
	
	gClientApplication.reset();
}
