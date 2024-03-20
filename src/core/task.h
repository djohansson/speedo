#pragma once

#include "taskstate.h"
#include "utils.h"

#include <memory>
#include <optional>
#include <tuple>
#include <type_traits>
#include <vector>

// TODO: Consider using dynamic memory allocation for callable and arguments if larger tasks are required. Currently, the maximum size is 56 bytes for the callable and 32 bytes for the arguments.
class Task : public Noncopyable
{
public:
	constexpr Task() noexcept = default;
	Task(Task&& other) noexcept;
	~Task();

	operator bool() const noexcept;
	bool operator!() const noexcept;
	Task& operator=(Task&& other) noexcept;
	template <typename... Args>
	void operator()(Args&&... args);

private:
	friend class TaskExecutor;

	template <
		typename F,
		typename CallableType = std::decay_t<F>,
		typename... Args,
		typename ArgsTuple = std::tuple<Args...>,
		typename ReturnType = std::invoke_result_t<CallableType, Args...>>
	requires std::invocable<F&, Args...> Task(F&& f, Args&&... args);

	// template <class Tuple, size_t... I>
	// constexpr decltype(auto) apply(Tuple&& t, std::index_sequence<I...>)
	// {
	// 	return (*this)(std::get<I>(std::forward<Tuple>(t))...);
	// }

	auto& state() noexcept { return myState; }
	const auto& state() const noexcept { return myState; }

	static constexpr size_t kMaxCallableSizeBytes = 56;
	static constexpr size_t kMaxArgsSizeBytes = 32;

	alignas(intptr_t) std::byte myCallableMemory[kMaxCallableSizeBytes];
	alignas(intptr_t) std::byte myArgsMemory[kMaxArgsSizeBytes];
	void (*myInvokeFcnPtr)(const void*, const void*, void*){};
	void (*myCopyFcnPtr)(void*, const void*, void*, const void*){};
	void (*myDeleteFcnPtr)(void*, void*){};
	std::shared_ptr<TaskState> myState;
};

#include "task.inl"
