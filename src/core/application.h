#pragma once

#include <core/taskexecutor.h>
#include <core/utils.h>

#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace core
{

struct Environment
{
	using VariableValue = std::variant<std::string, std::filesystem::path, int64_t, float, bool>;

	UnorderedMap<std::string, VariableValue> variables;
};

class Application;
extern std::weak_ptr<Application> gApplication;
class Application
{
public:
	Application(const Application&) = delete;
	Application(Application&&) noexcept = delete;
	virtual ~Application() = default;

	Application& operator=(const Application&) = delete;
	Application& operator=(Application&&) noexcept = delete;

	[[nodiscard]] auto& GetName() noexcept { return myName; }
	[[nodiscard]] const auto& GetName() const noexcept { return myName; }

	[[nodiscard]] auto& GetEnv() noexcept { return myEnvironment; }
	[[nodiscard]] const auto& GetEnv() const noexcept { return myEnvironment; }

	[[nodiscard]] auto& GetExecutor() noexcept { return *myExecutor; }
	[[nodiscard]] const auto& GetExecutor() const noexcept { return *myExecutor; }

	void RequestExit() noexcept { myExitRequested = true; }
	[[nodiscard]] bool IsExitRequested() const noexcept { return myExitRequested; }

	[[nodiscard]] static std::shared_ptr<Application> Get() { return gApplication.lock(); }

protected:
	Application(std::string_view name, Environment&& env);

private:
	std::string myName;
	Environment myEnvironment;
	std::unique_ptr<TaskExecutor> myExecutor;
	std::atomic_bool myExitRequested = false;
};

// creates the application and publishes it in gApplication *before* constructing it, since objects created during
// construction resolve the application through Application::Get(). T is constructed exactly once, in place.
template <typename T, typename... Args>
[[nodiscard]] std::shared_ptr<T> CreateApplication(Args&&... args)
{
	static_assert(std::is_base_of_v<Application, T>);

	static constexpr auto kAlignment = std::align_val_t{alignof(T)};
	std::shared_ptr<T> app(
		static_cast<T*>(::operator new(sizeof(T), kAlignment)),
		[](T* ptr)
		{
			std::destroy_at(ptr);
			::operator delete(ptr, kAlignment);
		});

	gApplication = app;
	std::construct_at(app.get(), std::forward<Args>(args)...);

	return app;
}

} // namespace core
