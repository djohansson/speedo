#include "capi.h"

#include <rhi/rhiapplication.h>
#include <server/rpc.h>

#include <zmq.hpp>
#include <zmq_addon.hpp>

#include <array>
#include <cassert>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>

namespace client
{

using namespace std::literals;
using namespace zpp::bits::literals;

static std::pair<TaskHandle, Future<void>> g_rpcTask{};

void rpc(TaskExecutor& executor, zmq::socket_t& socket, zmq::active_poller_t& poller)
{
	ZoneScopedN("client::rpc");

	if (auto app = Application::instance().lock(); !app || app->exitRequested())
		return;

	try
	{
		std::array<std::byte, 64> responseData;
		std::array<std::byte, 64> requestData;

		zpp::bits::in in{responseData};
		zpp::bits::out out{requestData};

		server::rpc_say::client client{in, out};

		if (auto result = client.request<"say"_sha256_int>("hello"s); failure(result))
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
					auto responseResult = client.response<"say"_sha256_int>();
					responseFailure = failure(responseResult);
					if (responseFailure)
					{
						std::cerr << "client.response() returned error code: " << std::make_error_code(responseResult.error()).message() << std::endl;
						return;
					}
					
					//std::cout << "say(\"hello\") returned: " << responseResult.value() << std::endl;
				}
				else
				{
					std::cerr << "socket.recv() failed" << std::endl;
					return;
				}
			}

			if (auto app = Application::instance().lock(); !app || app->exitRequested())
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

	// todo: move to own function?
	{
		auto rpcPair = executor.createTask(rpc, executor, socket, poller);
		auto& [rpcTask, rpcFuture] = rpcPair;
		executor.addDependency(g_rpcTask.first, rpcTask, true);
		g_rpcTask = rpcPair;
	}
}

class Client : public RhiApplication
{	
public:
	~Client()
	{
		ZoneScopedN("Client::~Client");

		g_rpcTask.second.wait();
		g_rpcTask = {};

		myPoller.remove(mySocket);
		mySocket.close();
		myContext.shutdown();
		myContext.close();

		std::cout << "Client shutting down, goodbye." << std::endl;
	}

	void tick() override
	{
		ZoneScopedN("Client::tick");

		RhiApplication::tick();
	}

protected:
	explicit Client() = default;
	Client(std::string_view name, Environment&& env, const WindowState& window)
	: RhiApplication(
		std::forward<std::string_view>(name),
		std::forward<Environment>(env),
		window)
	, myContext(1)
	, mySocket(myContext, zmq::socket_type::req)
	{
		auto& executor = internalExecutor();

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

static std::shared_ptr<Client> s_application{};

std::filesystem::path
getCanonicalPath(const char* pathStr, const char* defaultPathStr, bool createIfMissing = false)
{
	assert(defaultPathStr != nullptr);
	
	auto path = std::filesystem::path(pathStr ? pathStr : defaultPathStr);

	if (createIfMissing && !std::filesystem::exists(path))
		std::filesystem::create_directory(path);

	assert(std::filesystem::is_directory(path));

	return std::filesystem::canonical(path);
}

} // namespace client

void client_create(const WindowState* window, const PathConfig* paths)
{
	using namespace client;

	assert(window != nullptr);
	assert(window->handle != nullptr);
	assert(paths != nullptr);

	auto root = getCanonicalPath(nullptr, "./");

	s_application = Application::create<Client>(
		"client",
		Environment{{
			{"RootPath", root},
			{"ResourcePath", getCanonicalPath(paths->resourcePath, (root / "resources").string().c_str())},
			{"UserProfilePath", getCanonicalPath(paths->userProfilePath, (root / ".profile").string().c_str(), true)}
		}},
		*window);

	assert(s_application);
}

bool client_tick()
{
	using namespace client;

	assert(s_application);

	s_application->tick();

	return !s_application->exitRequested();
}

void client_resizeWindow(const WindowState* state)
{
	using namespace client;

	assert(state != nullptr);
	assert(s_application);
	
	s_application->onResizeWindow(*state);
}

void client_resizeFramebuffer(uint32_t width, uint32_t height)
{
	using namespace client;

	assert(s_application);
	
	s_application->onResizeFramebuffer(width, height);
}

void client_mouse(const MouseState* state)
{
	using namespace client;

	assert(state != nullptr);
	assert(s_application);
	
	s_application->onMouse(*state);
}

void client_keyboard(const KeyboardState* state)
{
	using namespace client;

	assert(state != nullptr);
	assert(s_application);
	
	s_application->onKeyboard(*state);
}

void client_destroy()
{
	using namespace client;

	assert(s_application);
	assert(s_application.use_count() == 1);
	
	s_application.reset();
}

const char* client_getAppName(void)
{
	using namespace client;

	assert(s_application);

	return s_application->name().data();
}
