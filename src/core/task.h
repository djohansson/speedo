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
	template <
		typename... Params,
		typename... Args,
		typename F,
		typename C = std::decay_t<F>,
		typename ArgsTuple = std::tuple<Args...>,
		typename ParamsTuple = std::tuple<Params...>,
		typename R = std_extra::apply_result_t<C, std_extra::tuple_cat_t<ArgsTuple, ParamsTuple>>>
	requires std_extra::applicable<C, std_extra::tuple_cat_t<ArgsTuple, ParamsTuple>>
	Task(F&& f, ParamsTuple&& params, Args&&... args);
	Task(Task&& other) noexcept;
	~Task();

	operator bool() const noexcept;
	bool operator!() const noexcept;
	Task& operator=(Task&& other) noexcept;
	template <typename... TaskParams>
	void operator()(TaskParams&&... params);

private:
	friend class TaskExecutor;

	auto& state() noexcept { return myState; }
	const auto& state() const noexcept { return myState; }

	static constexpr size_t kMaxCallableSizeBytes = 56;
	static constexpr size_t kMaxArgsSizeBytes = 32;

	alignas(intptr_t) std::array<std::byte, kMaxCallableSizeBytes> myCallableMemory;
	alignas(intptr_t) std::array<std::byte, kMaxArgsSizeBytes> myArgsMemory;
	void (*myInvokeFcnPtr)(const void*, const void*, void*, const void*){};
	void (*myCopyFcnPtr)(void*, const void*, void*, const void*){};
	void (*myDeleteFcnPtr)(void*, void*){};
	std::shared_ptr<TaskState> myState;
};

#include "task.inl"
