#include "capi.h"

#include <rhi/rhiapplication.h>

#define STB_SPRINTF_IMPLEMENTATION
#include <stb_sprintf.h>

#include <cassert>

namespace client
{

class ClientApplication : public RhiApplication
{	
public:
	~ClientApplication() = default;

	bool tick() override
	{
		++myTickCount;
		
		return RhiApplication::tick();
	}

protected:
	ClientApplication(std::string_view name, Environment&& env)
	: RhiApplication(std::forward<std::string_view>(name), std::forward<Environment>(env))
	{ }

private:
	uint64_t myTickCount = 0;
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
		"client",
		Environment{
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

	return s_application->name().data();
}
