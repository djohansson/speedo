#include <core/assert.h>
#include <core/eventhandlers.h>
#include <core/file.h>
#include <core/concurrentaccess.h>

#include <client/capi.h>
#include <client/client.h>

#include <server/rpc/rpc.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include <GLFW/glfw3.h>

namespace client
{

using namespace core;
using namespace core::file;
using namespace rhi;

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

static void Rpc(zmq::socket_t& socket, zmq::active_poller_t& poller)
{
	ZoneScopedN("client::Rpc");

	using namespace std::literals;
	using namespace zpp::bits::literals;

	if (gRpcTaskState == kTaskStateShuttingDown)
	{
		SetTaskDone(gRpcTaskState);
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
					SetTaskDone(gRpcTaskState);
					return;
				}
				
				//std::cout << "Say(\"hello\") returned: " << responseResult.value() << std::endl;
			}
			else
			{
				std::cerr << "socket.recv() failed" << '\n';
				SetTaskDone(gRpcTaskState);
				return;
			}
		}

		if (gRpcTaskState == kTaskStateShuttingDown)
		{
			SetTaskDone(gRpcTaskState);
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
		SetTaskDone(gTickTaskState);
		return;
	}

	gClientApplication.Read()->Tick();

	auto tickTask = CreateTask(Tick);
	AddDependency(gTickTask.handle, tickTask.handle, true);
	gTickTask = tickTask;
}

static void Draw()
{
	ZoneScopedN("client::Draw");

	if (gDrawTaskState == kTaskStateShuttingDown)
	{
		SetTaskDone(gDrawTaskState);
		return;
	}

	// nothing was presented (e.g. minimized): back off instead of spinning the draw chain
	if (!gClientApplication.Read()->Draw())
		std::this_thread::sleep_for(std::chrono::milliseconds(10));

	auto drawTask = CreateTask(Draw);
	AddDependency(gDrawTask.handle, drawTask.handle, true);
	gDrawTask = drawTask;
}

Client::~Client()
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
	auto start = std::chrono::high_resolution_clock::now();

	myTimestamps[1] = myTimestamps[0];
	myTimestamps[0] = start;

	float dt = (myTimestamps[1] - myTimestamps[0]).count();

	auto& input = myInput;
	input.dt = dt;
	input.mouse.lastPosition = input.mouse.position;
	input.mouse.scroll = glm::vec2(0.0F);
	
	unsigned eventsProcessed = 0;

	MouseEvent mouse;
	while (myMouseQueue.try_dequeue(mouse))
	{
		if ((mouse.flags & MouseEvent::kPosition) != 0)
		{
			input.mouse.position[0] = static_cast<float>(mouse.xpos);
			input.mouse.position[1] = static_cast<float>(mouse.ypos);
			input.mouse.insideWindow = mouse.insideWindow;
		}

		if ((mouse.flags & MouseEvent::kScroll) != 0)
		{
			input.mouse.scroll[0] += static_cast<float>(mouse.xoffset);
			input.mouse.scroll[1] += static_cast<float>(mouse.yoffset);
		}

		if ((mouse.flags & MouseEvent::kButton) != 0)
		{
			bool leftPressed = (mouse.button == GLFW_MOUSE_BUTTON_LEFT && mouse.action == GLFW_PRESS);
			bool rightPressed = (mouse.button == GLFW_MOUSE_BUTTON_RIGHT && mouse.action == GLFW_PRESS);
			bool middlePressed = (mouse.button == GLFW_MOUSE_BUTTON_MIDDLE && mouse.action == GLFW_PRESS);
			bool leftReleased = (mouse.button == GLFW_MOUSE_BUTTON_LEFT && mouse.action == GLFW_RELEASE);
			bool rightReleased = (mouse.button == GLFW_MOUSE_BUTTON_RIGHT && mouse.action == GLFW_RELEASE);
			bool middleReleased = (mouse.button == GLFW_MOUSE_BUTTON_MIDDLE && mouse.action == GLFW_RELEASE);

			if (leftPressed)
			{
				ASSERT(!leftReleased);

				input.mouse.leftDown = true;
				input.mouse.leftLastPressPosition[0] = input.mouse.position[0];
				input.mouse.leftLastPressPosition[1] = input.mouse.position[1];
			}
			if (leftReleased)
			{
				ASSERT(!leftPressed);

				input.mouse.leftDown = false;
				input.mouse.leftLastPressPosition[0] = input.mouse.position[0];
				input.mouse.leftLastPressPosition[1] = input.mouse.position[1];
			}
			if (rightPressed)
			{
				ASSERT(!rightReleased);

				input.mouse.rightDown = true;
				input.mouse.rightLastPressPosition[0] = input.mouse.position[0];
				input.mouse.rightLastPressPosition[1] = input.mouse.position[1];
			}
			if (rightReleased)
			{
				ASSERT(!rightPressed);

				input.mouse.rightDown = false;
				input.mouse.rightLastPressPosition[0] = input.mouse.position[0];
				input.mouse.rightLastPressPosition[1] = input.mouse.position[1];
			}
			if (middlePressed)
			{
				ASSERT(!middleReleased);

				input.mouse.middleDown = true;
				input.mouse.middleLastPressPosition[0] = input.mouse.position[0];
				input.mouse.middleLastPressPosition[1] = input.mouse.position[1];
			}
			if (middleReleased)
			{
				ASSERT(!middlePressed);

				input.mouse.middleDown = false;
				input.mouse.middleLastPressPosition[0] = input.mouse.position[0];
				input.mouse.middleLastPressPosition[1] = input.mouse.position[1];
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

	if (eventsProcessed > 0 || input.keyboard.keysDown.any())
		WindowedApplication::OnInputStateChanged(input);

	WindowedApplication::PrepareDraw();

	using namespace std::chrono_literals;
	static constexpr std::chrono::microseconds kTickMinTime = 1000us;
	std::this_thread::sleep_until(start + kTickMinTime);
}

bool Client::Main()
{
	ZoneScopedN("Client::Main");

	return WindowedApplication::Main();
}

Client::Client(std::string_view name, Environment&& env, CreateWindowFunc createWindowFunc)
: WindowedApplication(
	std::forward<std::string_view>(name),
	std::forward<Environment>(env),
	createWindowFunc)
, myContext(1)
, mySocket(myContext, zmq::socket_type::req)
{
	AddMouseHandler(std::dynamic_pointer_cast<MouseEventHandler>(Application::Get()));
	AddKeyboardHandler(std::dynamic_pointer_cast<KeyboardEventHandler>(Application::Get()));

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

	gRpcTask = CreateTask(Rpc, mySocket, myPoller);
	gRpcTaskState = kTaskStateRunning;
	gTickTask = CreateTask(client::Tick);
	gTickTaskState = kTaskStateRunning;
	gDrawTask = CreateTask(client::Draw);
	gDrawTaskState = kTaskStateRunning;

	myTimestamps[0] = std::chrono::high_resolution_clock::now();

	// initial OnInputStateChanged call required to initialize data structures in imgui (and potentially others)
	// since WindowedApplication draw thread/tasks can launch before next Tick is called
	WindowedApplication::OnInputStateChanged(myInput);
}

} // namespace client

bool ClientMain()
{
	using namespace client;

	return gClientApplication.Read()->Main();
}

void ClientCreate(CreateWindowFunc createWindowFunc, const PathConfig* paths)
{
	using namespace client;

	ENSURE(paths != nullptr);

	auto root = GetCanonicalPath(nullptr, "./");
	auto resourcePath = GetCanonicalPath(paths->resourcePath, (root.value() / "resources").string().c_str());
	auto userPath = GetCanonicalPath(paths->userProfilePath, (root.value() / ".speedo").string().c_str(), true);

	ENSURE(root);
	ENSURE(resourcePath);
	ENSURE(userPath);

	auto& appPtrRef = gClientApplication.Write().Get();
	appPtrRef = CreateApplication<Client>(
		"client",
		Environment{{
			{"RootPath", root.value()},
			{"ResourcePath", resourcePath.value()},
			{"UserProfilePath", userPath.value()},
		},},
		createWindowFunc);

	std::array<TaskHandle, 3> handles{gRpcTask.handle, gTickTask.handle, gDrawTask.handle};
	appPtrRef->GetExecutor().Submit(handles);
}

void ClientDestroy(DestroyWindowFunc destroyWindowFunc)
{
	using namespace client;

	// however the client is closing (window closed, interrupted, or the exit menu item), tell work that checks for it,
	// e.g. loads, to wind down
	gClientApplication.Read()->RequestExit();

	// request all chains to stop first, so they wind down concurrently
	bool rpcRunning = RequestTaskStop(gRpcTaskState);
	bool tickRunning = RequestTaskStop(gTickTaskState);
	bool drawRunning = RequestTaskStop(gDrawTaskState);

	if (rpcRunning)
		WaitTaskStopped(gRpcTaskState);
	if (tickRunning)
		WaitTaskStopped(gTickTaskState);
	if (drawRunning)
		WaitTaskStopped(gDrawTaskState);

	auto& appPtrRef = gClientApplication.Write().Get();

	ENSURE(appPtrRef);

	// the app owns the vulkan surfaces/swapchains of these windows, so destroy it before the native windows
	std::vector<WindowHandle> windows;
	windows.reserve(appPtrRef->GetWindowCount());
	for (uint32_t windowIt = 0; windowIt < appPtrRef->GetWindowCount(); windowIt++)
		windows.emplace_back(appPtrRef->GetWindow(windowIt));

	appPtrRef->Shutdown();

	// only now: tasks (e.g. loads) hold the application while they run, and Shutdown() has joined them
	ASSERT(appPtrRef.use_count() == 1);
	appPtrRef.reset();

	for (auto window : windows)
		destroyWindowFunc(window);
}
