#include <limits>

template <typename T, typename AtomicT>
constexpr CopyableAtomic<T, AtomicT>::CopyableAtomic(
	CopyableAtomic<T, AtomicT>::value_t val) noexcept
	: AtomicT(val)
{}

template <typename T, typename AtomicT>
CopyableAtomic<T, AtomicT>::CopyableAtomic(const CopyableAtomic& other) noexcept
{
	store(other.load(std::memory_order_acquire), std::memory_order_release);
}

template <typename T, typename AtomicT>
CopyableAtomic<T, AtomicT>&
CopyableAtomic<T, AtomicT>::operator=(const CopyableAtomic& other) noexcept
{
	store(other.load(std::memory_order_acquire), std::memory_order_release);

	return *this;
}

template <typename ValueT, uint32_t Alignment>
#if __cpp_lib_atomic_ref >= 201806
auto UpgradableSharedMutex<ValueT, Alignment>::internalAtomicRef() noexcept
{
	return std::atomic_ref(myBits);
}
#else
auto& UpgradableSharedMutex<ValueT, Alignment>::internalAtomicRef() noexcept
{
	return myAtomic;
}
#endif

template <typename ValueT, uint32_t Alignment>
template <typename Func>
void UpgradableSharedMutex<ValueT, Alignment>::internalAquireLock(Func lockFn) noexcept
{
	auto result = lockFn();
	auto& [success, value] = result;
	while (!success)
	{
		internalAtomicRef().wait(value);
		result = lockFn();
	}
}

template <typename ValueT, uint32_t Alignment>
void UpgradableSharedMutex<ValueT, Alignment>::lock() noexcept
{
	internalAquireLock([this]() { return try_lock(); });
}

template <typename ValueT, uint32_t Alignment>
void UpgradableSharedMutex<ValueT, Alignment>::unlock() noexcept
{
	static_assert(Reader > Writer + Upgraded, "wrong bits!");

	internalAtomicRef().fetch_and(~(Writer | Upgraded), std::memory_order_release);
	internalAtomicRef().notify_all();
}

template <typename ValueT, uint32_t Alignment>
void UpgradableSharedMutex<ValueT, Alignment>::lock_shared() noexcept
{
	internalAquireLock([this]() { return try_lock_shared(); });
}

template <typename ValueT, uint32_t Alignment>
void UpgradableSharedMutex<ValueT, Alignment>::unlock_shared() noexcept
{
	internalAtomicRef().fetch_add(-Reader, std::memory_order_release);
	internalAtomicRef().notify_all();
}

template <typename ValueT, uint32_t Alignment>
void UpgradableSharedMutex<ValueT, Alignment>::unlock_and_lock_shared() noexcept
{
	internalAtomicRef().fetch_add(Reader, std::memory_order_acquire);

	unlock();
}

template <typename ValueT, uint32_t Alignment>
void UpgradableSharedMutex<ValueT, Alignment>::lock_upgrade() noexcept
{
	internalAquireLock([this]() { return try_lock_upgrade(); });
}

template <typename ValueT, uint32_t Alignment>
void UpgradableSharedMutex<ValueT, Alignment>::unlock_upgrade() noexcept
{
	internalAtomicRef().fetch_add(-Upgraded, std::memory_order_acq_rel);
	internalAtomicRef().notify_all();
}

template <typename ValueT, uint32_t Alignment>
void UpgradableSharedMutex<ValueT, Alignment>::unlock_upgrade_and_lock() noexcept
{
	// try to unlock upgrade and write lock atomically
	internalAquireLock([this]() { return try_lock<Upgraded>(); });
}

template <typename ValueT, uint32_t Alignment>
void UpgradableSharedMutex<ValueT, Alignment>::unlock_upgrade_and_lock_shared() noexcept
{
	internalAtomicRef().fetch_add(Reader - Upgraded, std::memory_order_acq_rel);
	internalAtomicRef().notify_all();
}

template <typename ValueT, uint32_t Alignment>
void UpgradableSharedMutex<ValueT, Alignment>::unlock_and_lock_upgrade() noexcept
{
	// need to do it in two steps here -- as the Upgraded bit might be OR-ed at
	// the same time when other threads are trying do try_lock_upgrade().

	internalAtomicRef().fetch_or(Upgraded, std::memory_order_acquire);
	internalAtomicRef().fetch_add(-Writer, std::memory_order_release);
	internalAtomicRef().notify_all();
}

template <typename ValueT, uint32_t Alignment>
template <typename UpgradableSharedMutex<ValueT, Alignment>::value_t Expected>
std::tuple<bool, typename UpgradableSharedMutex<ValueT, Alignment>::value_t>
UpgradableSharedMutex<ValueT, Alignment>::try_lock() noexcept
{
	auto result = std::make_tuple(false, Expected);
	auto& [success, value] = result;
	success = internalAtomicRef().compare_exchange_weak(value, Writer, std::memory_order_acq_rel);
	return result;
}

template <typename ValueT, uint32_t Alignment>
std::tuple<bool, typename UpgradableSharedMutex<ValueT, Alignment>::value_t>
UpgradableSharedMutex<ValueT, Alignment>::try_lock_shared() noexcept
{
	// fetch_add is considerably (100%) faster than compare_exchange,
	// so here we are optimizing for the common (lock success) case.
	value_t value = internalAtomicRef().fetch_add(Reader, std::memory_order_acquire);
	if (value & (Writer | Upgraded))
	{
		value = internalAtomicRef().fetch_add(-Reader, std::memory_order_release);
		return std::make_tuple(false, value);
	}
	return std::make_tuple(true, value);
}

template <typename ValueT, uint32_t Alignment>
std::tuple<bool, typename UpgradableSharedMutex<ValueT, Alignment>::value_t>
UpgradableSharedMutex<ValueT, Alignment>::try_lock_upgrade() noexcept
{
	value_t value = internalAtomicRef().fetch_or(Upgraded, std::memory_order_acquire);

	// Note: when failed, we cannot flip the Upgraded bit back,
	// as in this case there is either another upgrade lock or a write lock.
	// If it's a write lock, the bit will get cleared up when that lock's done
	// with unlock().
	return std::make_tuple(((value & (Upgraded | Writer)) == 0), value);
}

template <typename... Args>
void Task::operator()(Args&&... args)
{
	assertf(myInvokeFcnPtr, "Task is not initialized!");

	using ArgsTuple = std::tuple<Args...>;
	static_assert(sizeof(ArgsTuple) <= kMaxArgsSizeBytes);

	std::construct_at(
		static_cast<ArgsTuple*>(static_cast<void*>(myArgsMemory.data())),
		std::forward<Args>(args)...);

	myInvokeFcnPtr(myCallableMemory.data(), myArgsMemory.data(), myState.get());
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
	, myState(std::static_pointer_cast<TaskState>(std::make_shared<typename Future<ReturnType>::FutureState>()))
{
	static_assert(sizeof(Task) == 128);

	static_assert(sizeof(CallableType) <= kMaxCallableSizeBytes);
	std::construct_at(
		static_cast<CallableType*>(static_cast<void*>(myCallableMemory.data())),
		std::forward<CallableType>(f));

	static_assert(sizeof(ArgsTuple) <= kMaxArgsSizeBytes);
	std::construct_at(
		static_cast<ArgsTuple*>(static_cast<void*>(myArgsMemory.data())),
		std::forward<Args>(args)...);
}

template <typename T>
Future<T>::Future(std::shared_ptr<FutureState>&& state) noexcept
	: myState(std::forward<std::shared_ptr<FutureState>>(state))
{}

template <typename T>
Future<T>::Future(Future&& other) noexcept
	: myState(std::exchange(other.myState, {}))
{}

template <typename T>
Future<T>::Future(const Future& other) noexcept
	: myState(other.myState)
{}

template <typename T>
Future<T>& Future<T>::operator=(Future&& other) noexcept
{
	myState = std::exchange(other.myState, {});

	return *this;
}

template <typename T>
Future<T>& Future<T>::operator=(const Future& other) noexcept
{
	myState = other.myState;

	return *this;
}

template <typename T>
typename Future<T>::value_t Future<T>::get()
{
	wait();

	// important copy! otherwise value will be garbage on exit due to myState.reset().
	auto retval = myState->value;

	myState.reset();

	return retval;
}

template <typename T>
bool Future<T>::is_ready() const noexcept
{
	assertf(valid(), "Future is not valid!");

	return myState->latch.value().load(std::memory_order_acquire) == 0;
}

template <typename T>
bool Future<T>::valid() const noexcept
{
	return !!myState && myState->latch;
}

template <typename T>
void Future<T>::wait() const
{
	assertf(valid(), "Future is not valid!");

	while (true)
	{
		auto current = myState->latch.value().load(std::memory_order_acquire);

		if (current == 0)
			return;

		myState->latch.value().wait(current, std::memory_order_relaxed);
	}
}

// template <typename T>
// template <typename F, typename CallableType, typename... Args, typename ReturnType>
// requires std::invocable<F&, Args...> Future<ReturnType> Future<T>::then(F&& f)
// {
// 	auto& [value, flag, continuation] = *myState;

// 	continuation = Task(
// 		std::forward<CallableType>(f), std::make_shared<typename Future<ReturnType>::FutureState>());

// 	return Future<ReturnType>(
// 		continuation.template returnState<typename Future<ReturnType>::FutureState>());
// }

template <typename F, typename CallableType, typename... Args, typename ReturnType>
requires std::invocable<F&, Args...> std::tuple<TaskGraph::TaskNodeHandle, Future<ReturnType>>
TaskGraph::createTask(F&& f, Args&&... args)
{
	ZoneScopedN("TaskGraph::createTask");

	const auto& taskNode = myNodes.emplace_back(
		std::make_tuple(
			Task(std::forward<F>(f), std::forward<Args>(args)...),
			TaskNodeHandleVector{},
			0));
	
	const auto& [task, adjacencies, dependencies] = taskNode;

	return std::make_tuple(
		&taskNode - &myNodes[0],
		Future<ReturnType>(std::static_pointer_cast<typename Future<ReturnType>::FutureState>(task.state())));
}

template <typename ReturnType>
std::optional<typename Future<ReturnType>::value_t> TaskExecutor::join(Future<ReturnType>&& future)
{
	ZoneScopedN("TaskExecutor::join");

	auto result = processReadyQueue(std::forward<Future<ReturnType>>(future));

	return result;
}

template <typename ReturnType>
std::optional<typename Future<ReturnType>::value_t>
TaskExecutor::processReadyQueue(Future<ReturnType>&& future)
{
	ZoneScopedN("TaskExecutor::processReadyQueue");

	if (!future.valid())
		return std::nullopt;

	Task task;
	while (!future.is_ready() && myReadyQueue.try_dequeue(task))
	{
		task();
		scheduleAdjacent(task);
	}

	return std::make_optional(future.get());
}
