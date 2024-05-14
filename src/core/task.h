#pragma once

#include "capi.h"
#include "future.h"
#include "std_extra.h"

#include <memory>
#include <optional>
#include <tuple>
#include <type_traits>
#include <vector>

template <typename T>
using TaskCreateInfo = std::pair<TaskHandle, Future<T>>;

// TODO: Consider using dynamic memory allocation for callable and arguments if larger tasks are required. Currently, the maximum size is 56 bytes for the callable and 32 bytes for the arguments.
class Task : public Noncopyable
{
	friend class TaskExecutor;

public:
	template <
		typename... Params,
		typename... Args,
		typename F,
		typename C = std::decay_t<F>,
		typename ArgsTuple = std::tuple<Args...>,
		typename ParamsTuple = std::tuple<Params...>,
		typename R = std_extra::apply_result_t<C, std_extra::tuple_cat_t<ArgsTuple, ParamsTuple>>>
	requires std_extra::applicable<C, std_extra::tuple_cat_t<ArgsTuple, ParamsTuple>>
	static TaskCreateInfo<R> CreateTask(F&& callable, Args&&... args) noexcept;

	operator bool() const noexcept;
	Task& operator=(Task&& other) noexcept;
	template <typename... Params>
	void operator()(Params&&... params);

	// b will start after a has finished
	// isContinuation == true : b will most likely start on the same thread as a, but may start on any thread in the thread pool
	// isContinuation == false: b will start on any thread in the thread pool
	static void AddDependency(TaskHandle aTaskHandle, TaskHandle bTaskHandle, bool isContinuation = false);

private:
	constexpr Task() noexcept = default;
	Task(Task&& other) noexcept;
	~Task();
	template <
		typename... Params,
		typename... Args,
		typename F,
		typename C = std::decay_t<F>,
		typename ArgsTuple = std::tuple<Args...>,
		typename ParamsTuple = std::tuple<Params...>,
		typename R = std_extra::apply_result_t<C, std_extra::tuple_cat_t<ArgsTuple, ParamsTuple>>>
	requires std_extra::applicable<C, std_extra::tuple_cat_t<ArgsTuple, ParamsTuple>>
	constexpr Task(F&& callable, ParamsTuple&& params, Args&&... args) noexcept;

	auto& InternalState() noexcept { return myState; }
	const auto& InternalState() const noexcept { return myState; }

	static Task* InternalHandleToPtr(TaskHandle handle) noexcept;
	static TaskHandle InternalPtrToHandle(Task* ptr) noexcept;
	static TaskHandle InternalAllocate() noexcept;

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
