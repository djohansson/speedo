#include "volcano.h"
#include "application.h"

static std::unique_ptr<Application<GraphicsBackend::Vulkan>> theApplication;

int volcano_create(
	void* view, int width, int height, int framebufferWidth, int framebufferHeight,
	const char* resourcePath, const char* userProfilePath)
{
	assert(view != nullptr);

	theApplication = std::make_unique<Application<GraphicsBackend::Vulkan>>(
		view, width, height, framebufferWidth, framebufferHeight, resourcePath, userProfilePath);

	return EXIT_SUCCESS;
}

void volcano_draw()
{
	assert(theApplication);

	theApplication->draw();
}

void volcano_resizeWindow(const WindowState* state)
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

void volcano_mouse(const MouseState* state)
{
	assert(theApplication);
	assert(state != nullptr);

	theApplication->onMouse(*state);
}

void volcano_keyboard(const KeyboardState* state)
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

const char* volcano_getAppName(void)
{
	assert(theApplication);

	return theApplication->getName();
}