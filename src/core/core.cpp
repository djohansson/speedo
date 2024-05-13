#include "capi.h"
#include "application.h"

void UpdateMouse(const MouseEvent* state)
{
	if (auto app = Application::Instance().lock(); app)
		app->OnMouse(*state);
}

void UpdateKeyboard(const KeyboardEvent* state)
{
	if (auto app = Application::Instance().lock(); app)
		app->OnKeyboard(*state);
}

const char* GetApplicationName(void)
{
	if (auto app = Application::Instance().lock(); app)
		return app->Name().data();

	return nullptr;
}
