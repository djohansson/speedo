#include "volcano.h"
#include "application.h"

#include <filesystem>
#include <memory>
#include <string>

static std::unique_ptr<Application<Vk>> g_app;
static std::filesystem::path g_rootPath;
static std::filesystem::path g_resourcePath;
static std::filesystem::path g_userProfilePath;

namespace volcano
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

} // namespace volcano

int volcano_create(
	void* windowHandle,
	int width,
	int height,
	const char* rootPath,
	const char* resourcePath,
	const char* userProfilePath)
{
	assert(windowHandle != nullptr);

	g_rootPath = volcano::getCanonicalPath(resourcePath, "./");
	g_resourcePath = volcano::getCanonicalPath(resourcePath, "./resources/");
	g_userProfilePath = volcano::getCanonicalPath(userProfilePath, "./.profile/", true);
	g_app = std::make_unique<Application<Vk>>(windowHandle, width, height);

	return EXIT_SUCCESS;
}

bool volcano_draw()
{
	assert(g_app);

	return g_app->draw();
}

void volcano_resizeWindow(const WindowState* state)
{
	assert(g_app);
	assert(state != nullptr);

	g_app->resizeWindow(*state);
}

void volcano_resizeFramebuffer(int width, int height)
{
	assert(g_app);

	g_app->resizeFramebuffer(width, height);
}

void volcano_mouse(const MouseState* state)
{
	assert(g_app);
	assert(state != nullptr);

	g_app->onMouse(*state);
}

void volcano_keyboard(const KeyboardState* state)
{
	assert(g_app);
	assert(state != nullptr);

	g_app->onKeyboard(*state);
}

void volcano_destroy()
{
	assert(g_app);

	g_app.reset();
}

const char* volcano_getAppName(void)
{
	assert(g_app);

	return g_app->getName();
}

const char* volcano_getRootPath(void)
{
	static std::string rootPath;
	rootPath = g_rootPath.string();
	return rootPath.c_str();
}

const char* volcano_getResourcePath(void)
{
	static std::string resourcePath;
	resourcePath = g_resourcePath.string();
	return resourcePath.c_str();
}

const char* volcano_getUserProfilePath(void)
{
	static std::string userProfilePath;
	userProfilePath = g_userProfilePath.string();
	return userProfilePath.c_str();
}
