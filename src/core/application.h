#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <type_traits>

#include <core/utils.h>

class Application : public Noncopyable
{
public:
	virtual ~Application() = default;

	struct State
	{
		std::string name;
		std::filesystem::path rootPath;
		std::filesystem::path resourcePath;
		std::filesystem::path userProfilePath;
	};

	auto& state() noexcept { return myState; }
	const auto& state() const noexcept { return myState; }

	template <typename T, typename... Args>
	static std::shared_ptr<T> create(Args&&... args)
	{
		static_assert(std::is_base_of_v<Application, T>);
		struct U : public T { U(Args&&... args) : T(std::forward<Args>(args)...) {} }; // workaround for protected constructors in T
		auto app = std::make_shared<U>(std::forward<Args>(args)...);
		theApplication = app;
		return app;
	}

	static auto& get() noexcept { return theApplication; }

protected:
	Application(State&& state);

private:
	State myState{};
	static std::weak_ptr<Application> theApplication;
};
