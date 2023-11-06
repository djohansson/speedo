template <typename... Args>
void Task::operator()(Args&&... args)
{
	ZoneScopedN("Task::operator()");

	assertf(myInvokeFcnPtr, "Task is not initialized!");

	using ArgsTuple = std::tuple<Args...>;
	static_assert(sizeof(ArgsTuple) <= kMaxArgsSizeBytes);

	std::construct_at(
		static_cast<ArgsTuple*>(static_cast<void*>(myArgsMemory)),
		std::forward<Args>(args)...);

	myInvokeFcnPtr(myCallableMemory, myArgsMemory, myState.get());
}

template <
	typename F,
	typename CallableType,
	typename... Args,
	typename ArgsTuple,
	typename ReturnType>
requires std::invocable<F&, Args...> Task::Task(F&& f, Args&&... args)
	: myInvokeFcnPtr(
		[](const void* callablePtr, const void* argsPtr, void* returnPtr)
		{
			const auto& callable = *static_cast<const CallableType*>(callablePtr);
			const auto& args = *static_cast<const ArgsTuple*>(argsPtr);

			assertf(returnPtr, "Task::operator() called without any return state!");

			auto& state = *static_cast<typename Future<ReturnType>::FutureState*>(returnPtr);

			assertf(state.latch, "Latch needs to have been constructed!");

			if constexpr (std::is_void_v<ReturnType>)
				std::apply(callable, args);
			else
				state.value = std::apply(callable, args);

			auto counter = state.latch.value().fetch_sub(1, std::memory_order_release) - 1;
			(void)counter;
			assertf(counter == 0, "Latch counter should be zero!");

			state.latch.value().notify_all();

			//   if (continuation)
			//   {
			// 	  if constexpr (std::is_tuple_v<ReturnType>)
			// 		  continuation.apply(
			// 			  value,
			// 			  std::make_index_sequence<
			// 				  std::tuple_size_v<std::remove_reference_t<ArgsTuple>>>{});
			// 	  else
			// 		  continuation(value);
			//   }
		})
	, myCopyFcnPtr(
		  [](void* callablePtr,
			 const void* otherCallablePtr,
			 void* argsPtr,
			 const void* otherArgsPtr)
		  {
			  std::construct_at(
				  static_cast<CallableType*>(callablePtr),
				  *static_cast<const CallableType*>(otherCallablePtr));
			  std::construct_at(
				  static_cast<ArgsTuple*>(argsPtr), *static_cast<const ArgsTuple*>(otherArgsPtr));
		  })
	, myDeleteFcnPtr(
		  [](void* callablePtr, void* argsPtr)
		  {
			  std::destroy_at(static_cast<CallableType*>(callablePtr));
			  std::destroy_at(static_cast<ArgsTuple*>(argsPtr));
		  })
	, myState(std::static_pointer_cast<TaskState>(
		  std::make_shared<typename Future<ReturnType>::FutureState>()))
{
	static_assert(sizeof(Task) == 128);

	static_assert(sizeof(CallableType) <= kMaxCallableSizeBytes);
	std::construct_at(
		static_cast<CallableType*>(static_cast<void*>(myCallableMemory)),
		std::forward<CallableType>(f));

	static_assert(sizeof(ArgsTuple) <= kMaxArgsSizeBytes);
	std::construct_at(
		static_cast<ArgsTuple*>(static_cast<void*>(myArgsMemory)),
		std::forward<Args>(args)...);
}
