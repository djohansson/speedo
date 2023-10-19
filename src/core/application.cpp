#include "application.h"

#include <mimalloc-new-delete.h>

#define STB_SPRINTF_IMPLEMENTATION
#include <stb_sprintf.h>

#include <locale>

std::weak_ptr<Application> Application::theApplication{};

Application::Application(std::string_view name, Environment&& env)
: myName(name)
, myEnvironment(std::forward<Environment>(env))
{
	assertf(theApplication.use_count() == 0, "There can only be one application at a time");
	
	std::locale utf8Locale(".UTF8");  	
	std::locale::global(utf8Locale);
}
