#include "assert.h"//NOLINT(modernize-deprecated-headers)

Task* InternalHandleToPtr(TaskHandle handle) noexcept;
TaskHandle InternalPtrToHandle(Task* ptr) noexcept;
TaskHandle InternalAllocate() noexcept;
void InternalFree(TaskHandle handle) noexcept;

template <typename... Params, typename... Args, typename F, typename C, typename ArgsTuple, typename ParamsTuple, typename R>
requires std_extra::applicable<C, std_extra::tuple_cat_t<ArgsTuple, ParamsTuple>>
constexpr Task::Task(std::shared_ptr<TaskState>&& state, F&& callable, ParamsTuple&& params, Args&&... args) noexcept
	: myInvokeFcn(
		[](void* callablePtr, const void* argsPtr, void* statePtr, const void* paramsPtr)
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

			auto counter = state.latch.fetch_sub(1, std::memory_order_release) - 1;
			ASSERTF(counter == 0, "Latch counter should be zero!");

			state.latch.notify_all();
		})
	, myDeleteFcn(
		  [](void* callablePtr, void* argsPtr)
		  {
			  std::destroy_at(static_cast<C*>(callablePtr));
			  std::destroy_at(static_cast<ArgsTuple*>(argsPtr));
		  })
	, myState(std::forward<std::shared_ptr<TaskState>>(state))
{
#if defined(__cpp_lib_move_only_function) && __cpp_lib_move_only_function >= 201907L
	static constexpr auto kExpectedTaskSize = 128;
	static_assert(sizeof(Task) == kExpectedTaskSize);
#endif

	static_assert(sizeof(C) <= kMaxCallableSizeBytes);
	std::construct_at(
		static_cast<C*>(static_cast<void*>(myCallableMemory.data())),//NOLINT(bugprone-casting-through-void)
		std::forward<C>(callable));

	static_assert(sizeof(ArgsTuple) <= kMaxArgsSizeBytes);
	std::construct_at(
	 	static_cast<ArgsTuple*>(static_cast<void*>(myArgsMemory.data())),//NOLINT(bugprone-casting-through-void)
	 	std::forward<Args>(args)...);
}

template <typename... Params>
inline void Task::operator()(Params&&... params)
{
	ASSERTF(myInvokeFcn, "Task is not initialized!");
	ASSERT(!!InternalState());

	auto taskParams = std::make_tuple(std::forward<Params>(params)...);
	myInvokeFcn(myCallableMemory.data(), myArgsMemory.data(), myState.get(), &taskParams);
}

template <typename... Params, typename... Args, typename F, typename C, typename ArgsTuple, typename ParamsTuple, typename R>
requires std_extra::applicable<C, std_extra::tuple_cat_t<ArgsTuple, ParamsTuple>>
TaskCreateInfo<R> CreateTask(F&& callable, Args&&... args) noexcept
{
	if (auto handle = InternalAllocate())
	{
		auto* taskPtr = InternalHandleToPtr(handle);
		auto state = std::make_shared<typename Future<R>::FutureState>();
		state->mutex.lock();
		std::construct_at(
			taskPtr,
			std::static_pointer_cast<TaskState>(state),
			std::forward<F>(callable),
			ParamsTuple{},
			std::forward<Args>(args)...);
		state->mutex.unlock();

		return { .handle = handle, .future = Future<R>(std::move(state)) };
	}

	CHECKF(false, "Task::InternalAllocate() failed, pool is full?");

	return {};
}
