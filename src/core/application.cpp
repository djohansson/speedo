#include "application.h"

#include <core/assert.h>

#if defined(SPEEDO_USE_MIMALLOC)
#include <mimalloc-new-delete.h>
#endif

std::weak_ptr<Application> gApplication;

Application::Application(std::string_view name, Environment&& env)
: myName(name)
, myEnvironment(std::forward<Environment>(env))
, myExecutor(std::make_unique<TaskExecutor>(std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 2)))
{
	ENSUREF(gApplication.use_count() == 0, "There can only be one application at a time");
	std::set_terminate([]()
	{
		LOG_ERROR("Terminate handled called\n");
#if __cpp_exceptions >= 199711L
		try
		{
			if (std::exception_ptr eptr = std::current_exception(); eptr)
				std::rethrow_exception(eptr);

			LOG_ERROR("Exiting without exception\n");
		}
		catch (const std::exception& ex)
		{
			LOG_ERROR("Exception: {}\n", ex.what());
		}
		catch (...)
		{
			LOG_ERROR("Unknown exception caught\n");
		}
#endif
		std::exit(EXIT_FAILURE);
	});
}

const char* GetApplicationName(void)
{
	if (auto app = gApplication.lock(); app)
		return app->GetName().data();

	return nullptr;
}


