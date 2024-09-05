#pragma once

#include "capi.h"
#include "std_extra.h"
#include "upgradablesharedmutex.h"
#include "utils.h"

#include <functional>
#include <memory>
#include <optional>
#include <tuple>
#include <type_traits>
#include <vector>

#if !defined(__cpp_lib_function_ref) || __cpp_lib_function_ref < 202306L
#include <tl/function_ref.hpp>
#endif

// TODO(djohansson): Consider using dynamic memory allocation for callable and arguments if larger tasks are required.
class alignas (std_extra::hardware_destructive_interference_size) Task final
{
	friend class TaskExecutor;

public:
	constexpr Task() noexcept = delete;
	Task(const Task&) = delete;
	Task(Task&&) noexcept = delete;
	~Task();

	Task& operator=(const Task&) = delete;
	Task& operator=(Task&&) noexcept = delete;

	explicit operator bool() const noexcept;
	template <typename... Params>
	void operator()(Params&&... params);

	template <
		typename... Params,
		typename... Args,
		typename F,
		typename C = std::decay_t<F>,
		typename ArgsTuple = std::tuple<Args...>,
		typename ParamsTuple = std::tuple<Params...>,
		typename R = std_extra::apply_result_t<C, std_extra::tuple_cat_t<ArgsTuple, ParamsTuple>>>
	requires std_extra::applicable<C, std_extra::tuple_cat_t<ArgsTuple, ParamsTuple>>
	constexpr Task(std::shared_ptr<struct TaskState>&& state, F&& callable, ParamsTuple&& params, Args&&... args) noexcept;

	void AddDependency(Task& other, bool isContinuation = false);

private:
	[[nodiscard]] auto& InternalState() noexcept { return myState; }
	[[nodiscard]] const auto& InternalState() const noexcept { return myState; }

	static constexpr size_t kMaxCallableSizeBytes = 40;
	static constexpr size_t kMaxArgsSizeBytes = 32;

	alignas(intptr_t) std::array<std::byte, kMaxCallableSizeBytes> myCallableMemory;
	alignas(intptr_t) std::array<std::byte, kMaxArgsSizeBytes> myArgsMemory;

#if defined(__cpp_lib_function_ref) && __cpp_lib_function_ref >= 202306L
	std::function_ref<void(void*, const void*, void*, const void*)> myInvokeFcn;
	std::function_ref<void(void*, void*)> myDeleteFcn;
#else
	tl::function_ref<void(void*, const void*, void*, const void*)> myInvokeFcn;
	tl::function_ref<void(void*, void*)> myDeleteFcn;
#endif
	std::shared_ptr<TaskState> myState;
};

static constexpr std::size_t kTaskPoolSize = (1 << 10); // todo: make this configurable
using TaskHandle = MinSizeIndex<kTaskPoolSize>;

struct TaskState
{
	std::atomic_uint32_t latch{1U};

	UpgradableSharedMutex mutex; // Protects the variables below
	std::vector<TaskHandle> adjacencies;
	bool continuation = false;
	//
};

template <typename T>
class Future
{
public:
	using value_t = std::conditional_t<std::is_void_v<T>, std::nullptr_t, T>;
	struct FutureState : TaskState { value_t value; };

	constexpr Future() noexcept = default;
	explicit Future(std::shared_ptr<FutureState>&& state) noexcept;
	Future(Future&& other) noexcept;
	Future(const Future& other) noexcept;
	
	[[nodiscard]] bool operator==(const Future& other) const noexcept;
	Future& operator=(Future&& other) noexcept;
	Future& operator=(const Future& other) noexcept;

	[[nodiscard]] value_t Get();
	[[nodiscard]] bool IsReady() const noexcept;
	[[nodiscard]] bool Valid() const noexcept;
	void Wait() const;

private:
	std::shared_ptr<FutureState> myState;
};

template <typename T>
struct TaskCreateInfo 
{
	TaskHandle handle;
	Future<T> future;
};

template <
	typename... Params,
	typename... Args,
	typename F,
	typename C = std::decay_t<F>,
	typename ArgsTuple = std::tuple<Args...>,
	typename ParamsTuple = std::tuple<Params...>,
	typename R = std_extra::apply_result_t<C, std_extra::tuple_cat_t<ArgsTuple, ParamsTuple>>>
requires std_extra::applicable<C, std_extra::tuple_cat_t<ArgsTuple, ParamsTuple>>
[[nodiscard]] TaskCreateInfo<R> CreateTask(F&& callable, Args&&... args) noexcept;

// b will start after a has finished
void AddDependency(TaskHandle aTaskHandle, TaskHandle bTaskHandle, bool isContinuation = false);

#include "task.inl"
#include "future.inl"
