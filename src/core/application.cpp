#include "application.h"

#include <mimalloc-new-delete.h>

std::weak_ptr<Application> gApplication;

Application::Application(std::string_view name, Environment&& env)
: myName(name)
, myEnvironment(std::forward<Environment>(env))
, myExecutor(std::make_unique<TaskExecutor>(std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 3)))
{
	assertf(gApplication.use_count() == 1, "There can only be one application at a time");
}

void Application::onMouse(const MouseEvent& mouse)
{
	myMouseQueue.enqueue(mouse);
}

void Application::onKeyboard(const KeyboardEvent& keyboard)
{
	myKeyboardQueue.enqueue(keyboard);
}

void Application::internalUpdateInput()
{
	MouseEvent mouse;
	while (myMouseQueue.try_dequeue(mouse))
	{}

	KeyboardEvent keyboard;
	while (myKeyboardQueue.try_dequeue(keyboard))
	{}
}
