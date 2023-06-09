#include "client.h"

#include <rhi/application.h>

#include <filesystem>
#include <memory>
#include <string>

static std::unique_ptr<Application<Vk>> g_app;
static std::filesystem::path g_rootPath;
static std::filesystem::path g_resourcePath;
static std::filesystem::path g_userProfilePath;

namespace client
{

std::filesystem::path
getCanonicalPath(const char* pathStr, const char* defaultPathStr, bool createIfMissing = false)
{
	auto path = std::filesystem::path(pathStr ? pathStr : defaultPathStr);

	if (createIfMissing && !std::filesystem::exists(path))
		std::filesystem::create_directory(path);

	assert(std::filesystem::is_directory(path));

	return std::filesystem::canonical(path);
}

} // namespace client

int client_create(
	const WindowState* window,
	const char* rootPath,
	const char* resourcePath,
	const char* userProfilePath)
{
	assert(window != nullptr);
	assert(window->handle != nullptr);

	g_rootPath = client::getCanonicalPath(resourcePath, "./");
	g_resourcePath = client::getCanonicalPath(resourcePath, "./src/rhi/resources/");
	g_userProfilePath = client::getCanonicalPath(userProfilePath, "./.profile/", true);
	g_app = std::make_unique<Application<Vk>>(*window);

	return EXIT_SUCCESS;
}

bool client_tick()
{
	assert(g_app);

	return g_app->tick();
}

void client_resizeWindow(const WindowState* state)
{
	assert(g_app);
	assert(state != nullptr);

	g_app->resizeWindow(*state);
}

void client_resizeFramebuffer(int width, int height)
{
	assert(g_app);

	g_app->resizeFramebuffer(width, height);
}

void client_mouse(const MouseState* state)
{
	assert(g_app);
	assert(state != nullptr);

	g_app->onMouse(*state);
}

void client_keyboard(const KeyboardState* state)
{
	assert(g_app);
	assert(state != nullptr);

	g_app->onKeyboard(*state);
}

void client_destroy()
{
	assert(g_app);

	g_app.reset();
}

const char* client_getAppName(void)
{
	assert(g_app);

	return g_app->getName();
}

const char* client_getRootPath(void)
{
	static std::string rootPath;
	rootPath = g_rootPath.string();
	return rootPath.c_str();
}

const char* client_getResourcePath(void)
{
	static std::string resourcePath;
	resourcePath = g_resourcePath.string();
	return resourcePath.c_str();
}

const char* client_getUserProfilePath(void)
{
	static std::string userProfilePath;
	userProfilePath = g_userProfilePath.string();
	return userProfilePath.c_str();
}
