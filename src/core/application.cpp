#include "application.h"

#include <mimalloc-new-delete.h>

std::weak_ptr<Application> Application::theApplication{};

Application::Application(std::string_view name, Environment&& env)
: myName(name)
, myEnvironment(std::forward<Environment>(env))
{
	assertf(theApplication.use_count() == 0, "There can only be one application at a time");
}
