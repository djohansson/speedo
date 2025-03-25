#include "application.h"

#include <iostream>

#if defined(SPEEDO_USE_MIMALLOC)
#include <mimalloc-new-delete.h>
#endif

std::weak_ptr<Application> gApplication;

Application::Application(std::string_view name, Environment&& env)
: myName(name)
, myEnvironment(std::forward<Environment>(env))
, myExecutor(std::make_unique<TaskExecutor>(std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 2)))
{
	ASSERTF(gApplication.use_count() == 1, "There can only be one application at a time");
	std::set_terminate([]()
	{
		std::cerr << "Unknown exception caught\n";
		std::exit(EXIT_FAILURE);
	});
}

const char* GetApplicationName(void)
{
	if (auto app = Application::Get().lock(); app)
		return app->GetName().data();

	return nullptr;
}


