#include "application.h"

#include <exception>
#include <iostream>

#include <mimalloc-new-delete.h>

std::weak_ptr<Application> gApplication;

Application::Application(std::string_view name, Environment&& env)
: myName(name)
, myEnvironment(std::forward<Environment>(env))
, myExecutor(std::make_unique<TaskExecutor>(std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 3)))
{
	ASSERTF(gApplication.use_count() == 1, "There can only be one application at a time");
	std::set_terminate([]()
	{
		try
		{
			std::exception_ptr eptr{std::current_exception()};
			if (eptr)
			{
				std::rethrow_exception(eptr);
			}
			else
			{
				std::cerr << "Exiting without exception\n";
			}
		}
		catch (const std::exception& exc)
		{
			std::cerr << "Exception: " << exc.what() << '\n';
		}
		catch (...)
		{
			std::cerr << "Unknown exception caught\n";
		}
		std::exit(EXIT_FAILURE);
	});
}

void Application::OnMouse(const MouseEvent& mouse)
{
	myMouseQueue.enqueue(mouse);
}

void Application::OnKeyboard(const KeyboardEvent& keyboard)
{
	myKeyboardQueue.enqueue(keyboard);
}

void Application::InternalUpdateInput()
{
	MouseEvent mouse;
	while (myMouseQueue.try_dequeue(mouse))
	{}

	KeyboardEvent keyboard;
	while (myKeyboardQueue.try_dequeue(keyboard))
	{}
}
