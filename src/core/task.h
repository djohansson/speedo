#pragma once

#include "std_extra.h"
#include "utils.h"

#include <memory>
#include <tuple>
#include <type_traits>

#if !defined(__cpp_lib_function_ref) || __cpp_lib_function_ref < 202306L
#include <tl/function_ref.hpp>
#endif

template <typename T>
struct TaskCreateInfo;

class alignas (std::hardware_constructive_interference_size) Task final
{
	template <typename... Params, typename... Args, typename F, typename C, typename ArgsTuple, typename ParamsTuple, typename R>
	requires std_extra::applicable<C, std_extra::tuple_cat_t<ArgsTuple, ParamsTuple>>
	friend TaskCreateInfo<R> CreateTask(F&& callable, Args&&... args) noexcept;
	friend class TaskExecutor;

public:
	constexpr Task() noexcept = delete;
	Task(const Task&) = delete;
	Task(Task&&) noexcept = delete;
	~Task() noexcept;

	Task& operator=(const Task&) = delete;
	Task& operator=(Task&&) noexcept = delete;

	[[nodiscard]] explicit operator bool() const noexcept;

	template <typename... Params>
	constexpr void operator()(Params&&... params);

	void AddDependency(Task& other, bool isContinuation = false) noexcept;

private:
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

	[[nodiscard]] auto& InternalState() noexcept { return myState; }
	[[nodiscard]] const auto& InternalState() const noexcept { return myState; }

	static constexpr size_t kTaskSize = 256;
	static constexpr size_t kMaxCallableSizeBytes = ((kTaskSize == 256) ? 128 : ((kTaskSize == 128) ? 48 : 0));
	static constexpr size_t kMaxArgsSizeBytes = ((kTaskSize == 256) ? 80 : ((kTaskSize == 128) ? 32 : 0));

	alignas(16) std::array<std::byte, kMaxCallableSizeBytes> myCallableMemory;
	alignas(16) std::array<std::byte, kMaxArgsSizeBytes> myArgsMemory;

#if defined(__cpp_lib_function_ref) && __cpp_lib_function_ref >= 202306L
	alignas(8) std::function_ref<void(void*, const void*, void*, const void*)> myInvokeFcn;
	alignas(8) std::function_ref<void(void*, void*)> myDeleteFcn;
#else
	alignas(8) tl::function_ref<void(void*, const void*, void*, const void*)> myInvokeFcn;
	alignas(8) tl::function_ref<void(void*, void*)> myDeleteFcn;
#endif
	alignas(8) std::shared_ptr<struct TaskState> myState;
};

static constexpr std::size_t kTaskPoolSize = (1 << 10); // todo: make this configurable
using TaskHandle = MinSizeIndex<kTaskPoolSize>;

struct TaskState
{
	std::array<TaskHandle, 128> adjacencies;
	static constexpr auto kAligmnent = std::atomic_ref<uint16_t>::required_alignment;
	alignas(kAligmnent) uint16_t latch{1U};
	uint8_t adjacenciesCount : 7;
	uint8_t continuation : 1;
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
	[[maybe_unused]] Future& operator=(Future&& other) noexcept;
	[[maybe_unused]] Future& operator=(const Future& other) noexcept;

	[[nodiscard]] value_t Get();
	[[nodiscard]] bool IsReady() const noexcept;
	[[nodiscard]] bool Valid() const noexcept;
	void Wait() const;

private:
	[[nodiscard]] auto& InternalState() noexcept { return myState; }
	[[nodiscard]] const auto& InternalState() const noexcept { return myState; }

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
void AddDependency(TaskHandle aTaskHandle, TaskHandle bTaskHandle, bool isContinuation = false) noexcept;

#include "task.inl"
#include "future.inl"
