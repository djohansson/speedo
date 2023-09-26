#include "application.h"

#include <locale>

#include <mimalloc-new-delete.h>

ApplicationBase::ApplicationBase()
{
	ZoneScoped;
	
	std::locale utf8Locale(".UTF8");  	
	std::locale::global(utf8Locale);
}
