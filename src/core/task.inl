template <typename... Params, typename... Args, typename F, typename C, typename ArgsTuple, typename ParamsTuple, typename R>
requires std_extra::applicable<C, std_extra::tuple_cat_t<ArgsTuple, ParamsTuple>>
constexpr Task::Task(F&& callable, ParamsTuple&& params, Args&&... args) noexcept
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
{
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
}

template <typename... Params>
inline void Task::operator()(Params&&... params)
{
	ASSERTF(myInvokeFcnPtr, "Task is not initialized!");
	ASSERT(!!InternalState());

	auto taskParams = std::make_tuple(std::forward<Params>(params)...);
	myInvokeFcnPtr(myCallableMemory.data(), myArgsMemory.data(), myState.get(), &taskParams);
}

template <typename... Params, typename... Args, typename F, typename C, typename ArgsTuple, typename ParamsTuple, typename R>
requires std_extra::applicable<C, std_extra::tuple_cat_t<ArgsTuple, ParamsTuple>>
TaskCreateInfo<R> Task::CreateTask(F&& callable, Args&&... args) noexcept
{
	if (auto handle = Task::InternalAllocate())
	{
		auto* taskPtr = Task::InternalHandleToPtr(handle);
		auto state = std::make_shared<typename Future<R>::FutureState>();
		
		state->mutex.lock();
		new (taskPtr) Task(std::forward<F>(callable), ParamsTuple{}, std::forward<Args>(args)...);
		taskPtr->myState = std::static_pointer_cast<TaskState>(state);
		state->mutex.unlock();

		return std::make_pair(handle, Future<R>(std::move(state)));
	}

	TRAP(); // Fatal error, task pool is full.

	return {};
}
