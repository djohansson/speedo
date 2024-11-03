#include "assert.h"//NOLINT(modernize-deprecated-headers)

namespace core
{
namespace detail
{

Task* InternalHandleToPtr(TaskHandle handle) noexcept;
TaskHandle InternalPtrToHandle(Task* ptr) noexcept;
TaskHandle InternalAllocate() noexcept;
void InternalFree(TaskHandle handle) noexcept;

template <typename C, typename ArgsTuple, typename ParamsTuple, typename R>
static void InternalInvoke(void* callablePtr, const void* argsPtr, void* statePtr, const void* paramsPtr)
{
	auto& callable = *static_cast<C*>(callablePtr);
	const auto& args = *static_cast<const ArgsTuple*>(argsPtr);
	const auto& params = *static_cast<const ParamsTuple*>(paramsPtr);

	ASSERTF(statePtr, "Task::operator() called without any return state!");

	auto& state = *static_cast<typename Future<R>::FutureState*>(statePtr);

	if constexpr (std::is_void_v<R>)
		std::apply(callable, std::tuple_cat(args, params));
	else
		state.value = std::apply(callable, std::tuple_cat(args, params));

	auto counter = state.Latch().fetch_sub(1, std::memory_order_release) - 1;
	ASSERTF(counter == 0, "Latch counter should be zero!");

	state.Latch().notify_all();
}

template <typename C, typename ArgsTuple>
static void InternalDelete(void* callablePtr, void* argsPtr)
{
	std::destroy_at(static_cast<C*>(callablePtr));
	std::destroy_at(static_cast<ArgsTuple*>(argsPtr));
}

} // namespace detail

} // namespace core

template <typename... Params, typename... Args, typename F, typename C, typename ArgsTuple, typename ParamsTuple, typename R>
requires std_extra::applicable<C, std_extra::tuple_cat_t<ArgsTuple, ParamsTuple>>
constexpr Task::Task(F&& callable, ParamsTuple&& params, Args&&... args) noexcept
	: myInvokeFcn(core::detail::InternalInvoke<C, ArgsTuple, ParamsTuple, R>)
	, myDeleteFcn(core::detail::InternalDelete<C, ArgsTuple>)
//	, myState(std::static_pointer_cast<TaskState>(std::make_shared<typename Future<R>::FutureState>()))
{
	std::atomic_store(&InternalState(), std::static_pointer_cast<TaskState>(std::make_shared<typename Future<R>::FutureState>()));

	static constexpr auto kExpectedTaskSize = 128;
	static_assert(sizeof(Task) == kExpectedTaskSize);

	static_assert(sizeof(C) <= kMaxCallableSizeBytes);
	std::construct_at(
		static_cast<C*>(static_cast<void*>(myCallableMemory.data())),//NOLINT(bugprone-casting-through-void)
		std::forward<C>(callable));

	static_assert(sizeof(ArgsTuple) <= kMaxArgsSizeBytes);
	std::construct_at(
	 	static_cast<ArgsTuple*>(static_cast<void*>(myArgsMemory.data())),//NOLINT(bugprone-casting-through-void)
	 	std::forward<Args>(args)...);

	static_assert(sizeof(Task) == kExpectedTaskSize);
}

template <typename... Params>
inline constexpr void Task::operator()(Params&&... params)
{
	ASSERT(*this);

	auto taskParams = std::make_tuple(std::forward<Params>(params)...);
	myInvokeFcn(myCallableMemory.data(), myArgsMemory.data(), std::atomic_load(&InternalState()).get(), &taskParams);
}

template <typename... Params, typename... Args, typename F, typename C, typename ArgsTuple, typename ParamsTuple, typename R>
requires std_extra::applicable<C, std_extra::tuple_cat_t<ArgsTuple, ParamsTuple>>
TaskCreateInfo<R> CreateTask(F&& callable, Args&&... args) noexcept
{
	if (auto handle = core::detail::InternalAllocate())
	{
		auto& task = *core::detail::InternalHandleToPtr(handle);

		new (&task) Task(
			std::forward<F>(callable),
			ParamsTuple{},
			std::forward<Args>(args)...);

		return { .handle = handle, .future = Future<R>(
			std::static_pointer_cast<typename Future<R>::FutureState>(std::atomic_load(&task.InternalState()))) };
	}

	CHECKF(false, "Task::InternalAllocate() failed, pool is full?");

	return {};
}
