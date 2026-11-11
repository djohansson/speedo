#include <core/application.h>
#include <core/assert.h>

#include <atomic>
#include <charconv>
#include <cstdlib>
#include <string_view>

#if defined(SPEEDO_USE_MIMALLOC)
#include <mimalloc-new-delete.h>
#endif

namespace core
{

std::weak_ptr<Application> gApplication;

Application::Application(std::string_view name, Environment&& env)
: myName(name)
, myEnvironment(std::forward<Environment>(env))
, myExecutor(std::make_unique<TaskExecutor>([]
	{
		// SPEEDO_EXECUTOR_THREAD_COUNT gives a small machine's executor (a 4-core runner's is 2 threads), where tasks that
		// hold their thread starve the rest (the deadlock in Scene's WaitUntilReady)
		if (const char* value = std::getenv("SPEEDO_EXECUTOR_THREAD_COUNT"); value != nullptr)
		{
			std::string_view text(value);
			uint32_t count = 0;
			if (auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), count);
				error == std::errc{} && end == text.data() + text.size() && count > 0)
				return count;
		}
		return static_cast<uint32_t>(std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 2));
	}()))
{
	std::set_terminate([]()
	{
		// once: std::exit's static destructors may terminate again (as may the logging), and each time this ran again,
		// recursing until the stack ran out, a stack trace printed at each level: a client sent SIGTERM as it started spun
		// for minutes, holding the gpu through the next runs
		static std::atomic_flag terminating;
		if (terminating.test_and_set())
			std::_Exit(EXIT_FAILURE);
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

} // namespace core

extern "C" const char* GetApplicationName(void)
{
	if (auto app = core::Application::Get(); app)
		return app->GetName().data();

	return "";
}
