#include "future.h"

template <typename... Params, typename... Args, typename F, typename C, typename ArgsTuple, typename ParamsTuple, typename R>
requires std_extra::applicable<C, std_extra::tuple_cat_t<ArgsTuple, ParamsTuple>>
Task::Task(F&& callable, ParamsTuple&& params, Args&&... args)
	: myInvokeFcnPtr(
		[](const void* callablePtr, const void* argsPtr, void* statePtr, const void* paramsPtr)
		{
			const auto& callable = *static_cast<const C*>(callablePtr);
			const auto& args = *static_cast<const ArgsTuple*>(argsPtr);
			const auto& params = *static_cast<const ParamsTuple*>(paramsPtr);

			ASSERTF(statePtr, "Task::operator() called without any return state!");

			auto& state = *static_cast<typename Future<R>::FutureState*>(statePtr);

			if constexpr (std::is_void_v<R>)
				apply(callable, std::tuple_cat(args, params));
			else
				state.value = apply(callable, std::tuple_cat(args, params));

			auto counter = state.latch.fetch_sub(1, std::memory_order_release) - 1;
			(void)counter;
			ASSERTF(counter == 0, "Latch counter should be zero!");

			state.latch.notify_all();
		})
	, myCopyFcnPtr(
		  [](void* callablePtr,
			 const void* otherCallablePtr,
			 void* argsPtr,
			 const void* otherArgsPtr)
		  {
			  std::construct_at(
				  static_cast<C*>(callablePtr),
				  *static_cast<const C*>(otherCallablePtr));
			  std::construct_at(
				  static_cast<ArgsTuple*>(argsPtr), *static_cast<const ArgsTuple*>(otherArgsPtr));
		  })
	, myDeleteFcnPtr(
		  [](void* callablePtr, void* argsPtr)
		  {
			  std::destroy_at(static_cast<C*>(callablePtr));
			  std::destroy_at(static_cast<ArgsTuple*>(argsPtr));
		  })
	, myState(std::static_pointer_cast<TaskState>(
		  std::make_shared<typename Future<R>::FutureState>()))
{
	static_assert(sizeof(Task) == 128);

	static_assert(sizeof(C) <= kMaxCallableSizeBytes);
	std::construct_at(
		static_cast<C*>(static_cast<void*>(myCallableMemory.data())),
		std::forward<C>(callable));

	static_assert(sizeof(ArgsTuple) <= kMaxArgsSizeBytes);
	std::construct_at(
	 	static_cast<ArgsTuple*>(static_cast<void*>(myArgsMemory.data())),
	 	std::forward<Args>(args)...);
}

template <typename... TaskParams>
void Task::operator()(TaskParams&&... params)
{
	ZoneScopedN("Task::operator()");

	ASSERTF(myInvokeFcnPtr, "Task is not initialized!");

	auto taskParams = std::make_tuple(std::forward<TaskParams>(params)...);
	myInvokeFcnPtr(myCallableMemory.data(), myArgsMemory.data(), myState.get(), &taskParams);
}
