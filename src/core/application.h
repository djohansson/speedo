#pragma once

#include <core/concurrency-utils.h>
#include <core/utils.h>

#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>

class Application : public Noncopyable, Nonmovable
{
public:
	struct State
	{
		std::string name;
		std::filesystem::path rootPath;
		std::filesystem::path resourcePath;
		std::filesystem::path userProfilePath;
	};

	virtual ~Application() = default;

	virtual bool tick() = 0;

	const auto& getState() const noexcept { return myState; }

	template <typename T, typename... Args>
	static std::shared_ptr<T> create(Args&&... args)
	{
		static_assert(std::is_base_of_v<Application, T>);
		struct U : public T { U(Args&&... args) : T(std::forward<Args>(args)...) {} }; // workaround for protected constructors in T
		auto app = std::make_shared<U>(std::forward<Args>(args)...);
		theApplication = app;
		return app;
	}

	auto& executor() noexcept { return myExecutor; }
	const auto& executor() const noexcept { return myExecutor; }

	static auto& instance() noexcept { return theApplication; }

protected:
	Application(State&& state);

private:
	State myState{};
	TaskExecutor myExecutor{std::max(1u, std::thread::hardware_concurrency() - 1)};
	static std::weak_ptr<Application> theApplication;
};
