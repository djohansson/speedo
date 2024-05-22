#pragma once

#include "capi.h"
#include "taskexecutor.h"
#include "utils.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <variant>

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
	virtual ~Application() noexcept(false) = default;

	Application& operator=(const Application&) = delete;
	Application& operator=(Application&&) noexcept = delete;

	virtual void Tick() { InternalUpdateInput(); };

	template <typename T, typename... Args>
	static std::shared_ptr<T> Create(Args&&... args)
	{
		static_assert(std::is_base_of_v<Application, T>);

		 // workaround for protected constructors in T
		struct U : public T { constexpr U() = default;
			explicit U(Args&&... args) : T(std::forward<Args>(args)...) {}
		};

		// silly dance to make Application::Instance() work during construction
		auto app = std::make_shared_for_overwrite<U>();
		gApplication = app;
		std::construct_at(app.get(), std::forward<Args>(args)...);

		return app;
	}

	[[nodiscard]] auto& Name() noexcept { return myName; }
	[[nodiscard]] const auto& Name() const noexcept { return myName; }

	[[nodiscard]] auto& Env() noexcept { return myEnvironment; }
	[[nodiscard]] const auto& Env() const noexcept { return myEnvironment; }

	[[nodiscard]] auto& Executor() noexcept { return *myExecutor; }
	[[nodiscard]] const auto& Executor() const noexcept { return *myExecutor; }

	[[nodiscard]] static auto& Instance() noexcept { return gApplication; }

	void RequestExit() noexcept { myExitRequested = true; }
	[[nodiscard]] bool IsExitRequested() const noexcept { return myExitRequested; }

	void OnMouse(const MouseEvent& mouse);
	void OnKeyboard(const KeyboardEvent& keyboard);

protected:
	explicit Application() = default;
	Application(std::string_view name, Environment&& env);

	virtual void InternalUpdateInput();

	ConcurrentQueue<MouseEvent> myMouseQueue;
	ConcurrentQueue<KeyboardEvent> myKeyboardQueue;

private:
	std::string myName;
	Environment myEnvironment;
	std::unique_ptr<TaskExecutor> myExecutor;
	std::atomic_bool myExitRequested = false;
};
