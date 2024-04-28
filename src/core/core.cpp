#include "capi.h"
#include "application.h"

void core_mouse(const MouseEvent* state)
{
	if (auto app = Application::instance().lock(); app)
		app->onMouse(*state);
}

void core_keyboard(const KeyboardEvent* state)
{
	if (auto app = Application::instance().lock(); app)
		app->onKeyboard(*state);
}

const char* core_getAppName(void)
{
	if (auto app = Application::instance().lock(); app)
		return app->name().data();

	return nullptr;
}
