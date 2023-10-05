#include "capi.h"

#include <rhi/graphicsapplication.h>

#include <cassert>

namespace client
{

static std::shared_ptr<GraphicsApplication<Vk>> s_application{};

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
	assert(window != nullptr);
	assert(window->handle != nullptr);

	client::s_application = Application::create<GraphicsApplication<Vk>>(
		Application::State{
			"client",
			client::getCanonicalPath(rootPath, "./"),
			client::getCanonicalPath(resourcePath, "./resources/"),
			client::getCanonicalPath(userProfilePath, "./.profile/", true)
		});

	assert(client::s_application);

	client::s_application->createDevice(*window);
}

bool client_tick()
{
	assert(client::s_application);

	return client::s_application->tick();
}

void client_resizeWindow(const WindowState* state)
{
	assert(state != nullptr);
	assert(client::s_application);
	
	client::s_application->resizeWindow(*state);
}

void client_resizeFramebuffer(uint32_t width, uint32_t height)
{
	assert(client::s_application);
	
	client::s_application->resizeFramebuffer(width, height);
}

void client_mouse(const MouseState* state)
{
	assert(state != nullptr);
	assert(client::s_application);
	
	client::s_application->onMouse(*state);
}

void client_keyboard(const KeyboardState* state)
{
	assert(state != nullptr);
	assert(client::s_application);
	
	client::s_application->onKeyboard(*state);
}

void client_destroy()
{
	assert(client::s_application);
	
	client::s_application.reset();
}

const char* client_getAppName(void)
{
	assert(client::s_application);

	return client::s_application->state().name.data();
}
