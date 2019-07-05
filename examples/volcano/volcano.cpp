#include "volcano.h"
#include "application.h"

static std::unique_ptr<Application<GraphicsBackend::Vulkan>> theApplication;

int volcano_create(
	void* view, int width, int height, int framebufferWidth, int framebufferHeight,
	const char* resourcePath)
{
	assert(view != nullptr);

	static const char* DISABLE_VK_LAYER_VALVE_steam_overlay_1 =
		"DISABLE_VK_LAYER_VALVE_steam_overlay_1=1";
#if defined(__WINDOWS__)
	_putenv((char*)DISABLE_VK_LAYER_VALVE_steam_overlay_1);
#else
	putenv((char*)DISABLE_VK_LAYER_VALVE_steam_overlay_1);
#endif

#ifdef _DEBUG
	static const char* VK_LOADER_DEBUG_STR = "VK_LOADER_DEBUG";
	if (char* vkLoaderDebug = getenv(VK_LOADER_DEBUG_STR))
		std::cout << VK_LOADER_DEBUG_STR << "=" << vkLoaderDebug << std::endl;

	static const char* VK_LAYER_PATH_STR = "VK_LAYER_PATH";
	if (char* vkLayerPath = getenv(VK_LAYER_PATH_STR))
		std::cout << VK_LAYER_PATH_STR << "=" << vkLayerPath << std::endl;

	static const char* VK_ICD_FILENAMES_STR = "VK_ICD_FILENAMES";
	if (char* vkIcdFilenames = getenv(VK_ICD_FILENAMES_STR))
		std::cout << VK_ICD_FILENAMES_STR << "=" << vkIcdFilenames << std::endl;
#endif

	theApplication = std::make_unique<Application<GraphicsBackend::Vulkan>>(
		view, width, height, framebufferWidth, framebufferHeight,
		resourcePath ? resourcePath : "./");

	return EXIT_SUCCESS;
}

void volcano_draw()
{
	assert(theApplication);

	theApplication->draw();
}

void volcano_resizeWindow(const window_state* state)
{
	assert(theApplication);
	assert(state != nullptr);

	theApplication->resizeWindow(*state);
}

void volcano_resizeFramebuffer(int width, int height)
{
	assert(theApplication);

	theApplication->resizeFramebuffer(width, height);
}

void volcano_mouse(const mouse_state* state)
{
	assert(theApplication);
	assert(state != nullptr);

	theApplication->onMouse(*state);
}

void volcano_keyboard(const keyboard_state* state)
{
	assert(theApplication);
	assert(state != nullptr);

	theApplication->onKeyboard(*state);
}

void volcano_destroy()
{
	assert(theApplication);

	theApplication.reset();
}
