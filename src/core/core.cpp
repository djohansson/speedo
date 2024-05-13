#include "capi.h"
#include "application.h"

void core_mouse(const MouseEvent* state)
{
	if (auto app = Application::Instance().lock(); app)
		app->OnMouse(*state);
}

void core_keyboard(const KeyboardEvent* state)
{
	if (auto app = Application::Instance().lock(); app)
		app->OnKeyboard(*state);
}

const char* core_getAppName(void)
{
	if (auto app = Application::Instance().lock(); app)
		return app->Name().data();

	return nullptr;
}
