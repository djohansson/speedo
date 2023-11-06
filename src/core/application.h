#pragma once

#include <core/concurrency-utils.h>
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

class Application : public Noncopyable, public Nonmovable
{
public:

	virtual ~Application() = default;

	virtual bool tick() = 0;

	template <typename T, typename... Args>
	static std::shared_ptr<T> create(Args&&... args)
	{
		static_assert(std::is_base_of_v<Application, T>);
		struct U : public T { U(Args&&... args) : T(std::forward<Args>(args)...) {} }; // workaround for protected constructors in T
		auto app = std::make_shared<U>(std::forward<Args>(args)...);
		theApplication = app;
		return app;
	}

	auto& name() noexcept { return myName; }
	const auto& name() const noexcept { return myName; }

	auto& environment() noexcept { return myEnvironment; }
	const auto& environment() const noexcept { return myEnvironment; }

	auto& executor() noexcept { return myExecutor; }
	const auto& executor() const noexcept { return myExecutor; }

	static auto& instance() noexcept { return theApplication; }

protected:
	Application(std::string_view name, Environment&& env);

private:
	std::string myName;
	Environment myEnvironment;
	TaskExecutor myExecutor{std::max(1u, std::thread::hardware_concurrency() - 1)};
	static std::weak_ptr<Application> theApplication;
};
