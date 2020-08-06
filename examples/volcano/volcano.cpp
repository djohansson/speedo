#include "volcano.h"
#include "application.h"

static std::unique_ptr<Application<Vk>> theApplication;

int volcano_create(
	void* view, int width, int height, const char* resourcePath, const char* userProfilePath)
{
	assert(view != nullptr);

	theApplication = std::make_unique<Application<Vk>>(
		view, width, height, resourcePath, userProfilePath);

	return EXIT_SUCCESS;
}

bool volcano_draw()
{
	assert(theApplication);

	return theApplication->draw();
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