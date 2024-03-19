#pragma once

#include <core/taskexecutor.h>
#include <core/utils.h>

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
extern std::weak_ptr<Application> theApplication;

class Application : public Noncopyable, Nonmovable
{
public:
	virtual ~Application() = default;

	virtual bool tick() = 0;

	template <typename T, typename... Args>
	static std::shared_ptr<T> create(Args&&... args)
	{
		static_assert(std::is_base_of_v<Application, T>);

		 // workaround for protected constructors in T
		struct U : public T { U() = default; U(Args&&... args) : T(std::forward<Args>(args)...) {} };
		
		// silly dance to make Application::instance() work during construction
		auto app = std::make_shared<U>();
		theApplication = app;
		std::construct_at(app.get(), std::forward<Args>(args)...);

		return app;
	}

	auto& name() noexcept { return myName; }
	const auto& name() const noexcept { return myName; }

	auto& environment() noexcept { return myEnvironment; }
	const auto& environment() const noexcept { return myEnvironment; }

	static auto& instance() noexcept { return theApplication; }

protected:
	explicit Application() = default;
	Application(std::string_view name, Environment&& env);

	auto& internalExecutor() noexcept { return *myExecutor; }
	const auto& internalExecutor() const noexcept { return *myExecutor; }

private:

	std::string myName;
	Environment myEnvironment;
	std::unique_ptr<TaskExecutor> myExecutor{};
	
};
