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

	virtual void OnEvent() { InternalTick(); };

	template <typename T, typename... Args>
	static std::shared_ptr<T> Create(Args&&... args);

	[[nodiscard]] auto& GetName() noexcept { return myName; }
	[[nodiscard]] const auto& GetName() const noexcept { return myName; }

	[[nodiscard]] auto& GetEnv() noexcept { return myEnvironment; }
	[[nodiscard]] const auto& GetEnv() const noexcept { return myEnvironment; }

	[[nodiscard]] auto& GetExecutor() noexcept { return *myExecutor; }
	[[nodiscard]] const auto& GetExecutor() const noexcept { return *myExecutor; }

	[[nodiscard]] static auto& Get() noexcept { return gApplication; }

	void RequestExit() noexcept { myExitRequested = true; }
	[[nodiscard]] bool IsExitRequested() const noexcept { return myExitRequested; }

	void OnMouse(const MouseEvent& mouse);
	void OnKeyboard(const KeyboardEvent& keyboard);

protected:
	explicit Application() = default;
	Application(std::string_view name, Environment&& env);

	virtual void InternalTick();

	ConcurrentQueue<MouseEvent> myMouseQueue;
	ConcurrentQueue<KeyboardEvent> myKeyboardQueue;

private:
	std::string myName;
	Environment myEnvironment;
	std::unique_ptr<TaskExecutor> myExecutor;
	std::atomic_bool myExitRequested = false;
};

#include "application.inl"