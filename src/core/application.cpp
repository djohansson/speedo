#include "application.h"

#include <mimalloc-new-delete.h>

#include <locale>

std::weak_ptr<Application> Application::theApplication{};

Application::Application(State&& state)
: myState(std::forward<State>(state))
{
	ZoneScoped;
	
	std::locale utf8Locale(".UTF8");  	
	std::locale::global(utf8Locale);
}
