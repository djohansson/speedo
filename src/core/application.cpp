#include "application.h"

#include <mimalloc-new-delete.h>

std::weak_ptr<Application> theApplication;

Application::Application(std::string_view name, Environment&& env)
: myName(name)
, myEnvironment(std::forward<Environment>(env))
, myExecutor(std::make_unique<TaskExecutor>(std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 3)))
{
	assertf(theApplication.use_count() == 1, "There can only be one application at a time");
}
