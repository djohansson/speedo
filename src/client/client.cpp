#include "capi.h"

#include <rhi/graphicsapplication.h>

#define STB_SPRINTF_IMPLEMENTATION
#include <stb_sprintf.h>

#include <cassert>

#include <hv/HttpClient.h>

namespace client
{

class ClientApplication : public GraphicsApplication<Vk>
{	
public:
	~ClientApplication() = default;

	bool tick() override
	{
		++myTickCount;
		
		if (myTickCount % 1000)
			printf("client says: tick! %llu\n", myTickCount);
		
		return GraphicsApplication<Vk>::tick();
	}

protected:
	ClientApplication(Application::State&& state)
	 : GraphicsApplication<Vk>(std::forward<Application::State>(state))
	{
		using namespace hv;

		HttpClient sync_client;
		HttpRequest req;
		req.method = HTTP_POST;
		req.url = "http://127.0.0.1:8080/echo";
		req.headers["Connection"] = "keep-alive";
		req.body = "This is a sync request.";
		req.timeout = 10;
		HttpResponse resp;
		int ret = sync_client.send(&req, &resp);
		if (ret != 0)
		{
			printf("request failed!\n");
		}
		else
		{
			printf("%d %s\r\n", resp.status_code, resp.status_message());
			printf("%s\n", resp.body.c_str());
		}
	}

private:
	uint64_t myTickCount = false;
};

static std::shared_ptr<ClientApplication> s_application{};

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

void client_create(
	const WindowState* window,
	const char* rootPath,
	const char* resourcePath,
	const char* userProfilePath)
{
	using namespace client;

	assert(window != nullptr);
	assert(window->handle != nullptr);

	s_application = Application::create<ClientApplication>(
		Application::State{
			"client",
			getCanonicalPath(rootPath, "./"),
			getCanonicalPath(resourcePath, "./resources/"),
			getCanonicalPath(userProfilePath, "./.profile/", true)
		});

	assert(s_application);

	s_application->createDevice(*window);
}

bool client_tick()
{
	using namespace client;

	assert(s_application);

	return s_application->tick();
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
	
	s_application.reset();
}

const char* client_getAppName(void)
{
	using namespace client;

	assert(s_application);

	return s_application->getState().name.data();
}
