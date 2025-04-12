#include "capi.h"
#include "client.h"

#include <core/assert.h>
#include <core/eventhandlers.h>
#include <core/file.h>
#include <core/concurrentaccess.h>
#include <core/upgradablesharedmutex.h>
#include <server/rpc/rpc.h>

#include <array>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>

#include <GLFW/glfw3.h>

#include <imgui.h>

namespace client
{

static TaskCreateInfo<void> gRpcTask, gTickTask, gDrawTask;
static ConcurrentAccess<std::shared_ptr<Client>> gClientApplication;
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

	ConcurrentReadScope(gClientApplication)->Tick();

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

	ConcurrentReadScope(gClientApplication)->Draw();

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

void Client::OnKeyboard(const KeyboardEvent& keyboard)
{
	ZoneScopedN("Client::OnKeyboard");

	myKeyboardQueue.enqueue(keyboard);
}

void Client::OnMouse(const MouseEvent& mouse)
{
	ZoneScopedN("Client::OnMouse");

	myMouseQueue.enqueue(mouse);
}

void Client::Tick()
{
	myTimestamps[1] = myTimestamps[0];
	myTimestamps[0] = std::chrono::high_resolution_clock::now();

	float dt = (myTimestamps[1] - myTimestamps[0]).count();

	auto& input = myInput;
	
	unsigned eventsProcessed = 0;

	MouseEvent mouse;
	while (myMouseQueue.try_dequeue(mouse))
	{
		if ((mouse.flags & MouseEvent::kPosition) != 0)
		{
			input.mouse.position[0] = static_cast<float>(mouse.xpos);
			input.mouse.position[1] = static_cast<float>(mouse.ypos);
			input.mouse.insideWindow = mouse.insideWindow;

			io.AddMousePosEvent(input.mouse.position[0], input.mouse.position[1]);
		}

		if ((mouse.flags & MouseEvent::kButton) != 0)
		{
			bool leftPressed = (mouse.button == GLFW_MOUSE_BUTTON_LEFT && mouse.action == GLFW_PRESS);
			bool rightPressed = (mouse.button == GLFW_MOUSE_BUTTON_RIGHT && mouse.action == GLFW_PRESS);
			bool leftReleased = (mouse.button == GLFW_MOUSE_BUTTON_LEFT && mouse.action == GLFW_RELEASE);
			bool rightReleased = (mouse.button == GLFW_MOUSE_BUTTON_RIGHT && mouse.action == GLFW_RELEASE);

			if (leftPressed)
			{
				input.mouse.leftDown = true;
				input.mouse.leftLastEventPosition[0] = input.mouse.position[0];
				input.mouse.leftLastEventPosition[1] = input.mouse.position[1];
				io.AddMouseButtonEvent(GLFW_MOUSE_BUTTON_LEFT, true);
			}
			else if (rightPressed)
			{
				input.mouse.rightDown = true;
				input.mouse.rightLastEventPosition[0] = input.mouse.position[0];
				input.mouse.rightLastEventPosition[1] = input.mouse.position[1];
				io.AddMouseButtonEvent(GLFW_MOUSE_BUTTON_RIGHT, true);
			}
			else if (leftReleased)
			{
				input.mouse.leftDown = false;
				input.mouse.leftLastEventPosition[0] = input.mouse.position[0];
				input.mouse.leftLastEventPosition[1] = input.mouse.position[1];
				io.AddMouseButtonEvent(GLFW_MOUSE_BUTTON_LEFT, false);
			}
			else if (rightReleased)
			{
				input.mouse.rightDown = false;
				input.mouse.rightLastEventPosition[0] = input.mouse.position[0];
				input.mouse.rightLastEventPosition[1] = input.mouse.position[1];
				io.AddMouseButtonEvent(GLFW_MOUSE_BUTTON_RIGHT, false);
			}
		}

		eventsProcessed++;
	}

	KeyboardEvent keyboard;
	while (myKeyboardQueue.try_dequeue(keyboard))
	{
		if (keyboard.action == GLFW_PRESS)
			input.keyboard.keysDown[keyboard.key] = true;
		else if (keyboard.action == GLFW_RELEASE)
			input.keyboard.keysDown[keyboard.key] = false;

		eventsProcessed++;
	}

	if (eventsProcessed > 0)
		RHIApplication::OnInputStateChanged(input);
}

bool Client::Main()
{
	ZoneScopedN("Client::Main");

	return RHIApplication::Main();
}

Client::Client(std::string_view name, Environment&& env, CreateWindowFunc createWindowFunc)
: RHIApplication(
	std::forward<std::string_view>(name),
	std::forward<Environment>(env),
	createWindowFunc)
, myContext(1)
, mySocket(myContext, zmq::socket_type::req)
{
	using namespace core;

	AddMouseHandler(std::dynamic_pointer_cast<MouseEventHandler>(Application::Get().lock()));
	AddKeyboardHandler(std::dynamic_pointer_cast<KeyboardEventHandler>(Application::Get().lock()));

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

	myTimestamps[0] = std::chrono::high_resolution_clock::now();

	// initial Tick call required to initialize data structures in imgui (and potentially others)
	// since RHIApplication draw thread/tasks can launch before next Tick is called
	Tick();
}

bool ClientMain()
{	
	using namespace client;

	return ConcurrentReadScope(gClientApplication)->Main();
}

void ClientCreate(CreateWindowFunc createWindowFunc, const PathConfig* paths)
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

	auto appPtr = ConcurrentWriteScope(gClientApplication);

	appPtr = Application::Create<Client>(
		"client",
		Environment{{
			{"RootPath", root.value()},
			{"ResourcePath", resourcePath.value()},
			{"UserProfilePath", userPath.value()}
		}},
		createWindowFunc);

	ASSERT(appPtr.Get());

	std::array<TaskHandle, 3> handles{gRpcTask.handle, gTickTask.handle, gDrawTask.handle};
	appPtr->GetExecutor().Submit(handles);
}

void ClientDestroy()
{
	using namespace client;

	gRpcTaskState = kTaskStateShuttingDown;
	gTickTaskState = kTaskStateShuttingDown;
	gDrawTaskState = kTaskStateShuttingDown;

	gRpcTaskState.wait(kTaskStateShuttingDown);
	gTickTaskState.wait(kTaskStateShuttingDown);
	gDrawTaskState.wait(kTaskStateShuttingDown);

	auto appPtr = ConcurrentWriteScope(gClientApplication);

	ASSERT(appPtr.Get());
	ASSERT(appPtr.Get().use_count() == 1);
	
	appPtr.Get().reset();
}
