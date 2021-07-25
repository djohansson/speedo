template <typename T, typename AtomicT>
constexpr CopyableAtomic<T, AtomicT>::CopyableAtomic(CopyableAtomic<T, AtomicT>::value_t val) noexcept
	: AtomicT(val)
{}

template <typename T, typename AtomicT>
CopyableAtomic<T, AtomicT>::CopyableAtomic(const CopyableAtomic& other) noexcept
{
	store(other.load(std::memory_order_acquire), std::memory_order_release);
}

template <typename T, typename AtomicT>
CopyableAtomic<T, AtomicT>& CopyableAtomic<T, AtomicT>::operator=(const CopyableAtomic& other) noexcept
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

template <typename T>
Future<T>::Future(std::shared_ptr<state_t>&& state) noexcept
	: myState(std::forward<std::shared_ptr<state_t>>(state))
{}

template <typename T>
Future<T>::Future(Future&& other) noexcept
	: myState(std::exchange(other.myState, {}))
{}

template <typename T>
Future<T>& Future<T>::operator=(Future&& other) noexcept
{
	myState = std::exchange(other.myState, {});

	return *this;
}

template <typename T>
typename Future<T>::value_t Future<T>::get()
{
	assert(valid());

	auto state = myState;
	auto& [value, flag] = *state;

	if (!is_ready())
		flag.wait(false, std::memory_order_acquire);

	myState.reset();

	return value;
}

template <typename T>
bool Future<T>::is_ready() const
{
	assert(valid());

	auto& [value, flag] = *myState;

	return flag.load(std::memory_order_acquire);
}

template <typename T>
bool Future<T>::valid() const
{
	return !!myState;
}

template <typename T>
void Future<T>::wait() const
{
	assert(valid());

	auto& [value, flag] = *myState;

	if (!is_ready())
		flag.wait(false, std::memory_order_acquire);
}

template <
	typename F, typename CallableType, typename... Args, typename ArgsTuple, typename ReturnType,
	typename ReturnState>
Task::Task(F&& f, Args&&... args)
	: myReturnState(std::make_shared<ReturnState>())
	, myInvokeFcnPtr([](void* callablePtr, void* argsPtr, void* returnPtr, void* continuationPtr) {
		assert(!continuationPtr);

		auto& callable = *static_cast<CallableType*>(callablePtr);
		auto& args = *static_cast<ArgsTuple*>(argsPtr);
		auto& [value, flag] = *static_cast<ReturnState*>(returnPtr);

		if constexpr (std::is_void_v<ReturnType>)
			std::apply(callable, args);
		else
			value = std::apply(callable, args);

		flag.store(true, std::memory_order_release);
		flag.notify_all();
	})
	, myMoveFcnPtr([](void* callablePtr, void* otherCallablePtr, void* argsPtr,
					  void* otherArgsPtr) {
		std::construct_at(
			static_cast<CallableType*>(callablePtr), *static_cast<CallableType*>(otherCallablePtr));
		std::destroy_at(static_cast<CallableType*>(otherCallablePtr));
		std::construct_at(static_cast<ArgsTuple*>(argsPtr), *static_cast<ArgsTuple*>(otherArgsPtr));
		std::destroy_at(static_cast<ArgsTuple*>(otherArgsPtr));
	})
	, myDeleteFcnPtr([](void* callablePtr, void* argsPtr) {
		std::destroy_at(static_cast<CallableType*>(callablePtr));
		std::destroy_at(static_cast<ArgsTuple*>(argsPtr));
	})
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

template <typename F, typename... Args>
TypedTask<F, Args...>::TypedTask(F&& f, Args&&... args)
	: Task(std::forward<F>(f), std::forward<Args>(args)...)
{}

template <typename F, typename... Args>
Future<typename TypedTask<F, Args...>::ReturnType> TypedTask<F, Args...>::getFuture()
{
	return Future<ReturnType>(
		std::static_pointer_cast<typename Future<ReturnType>::state_t>(myReturnState));
}

template <typename F, typename... Args>
void TypedTask<F, Args...>::invoke(Args&&... args)
{
	assert(myInvokeFcnPtr);

	std::destroy_at(static_cast<ArgsTuple*>(static_cast<void*>(myArgsMemory.data())));
	std::construct_at(
		static_cast<ArgsTuple*>(static_cast<void*>(myArgsMemory.data())),
		std::forward<Args>(args)...);

	myInvokeFcnPtr(
		myCallableMemory.data(), myArgsMemory.data(), myReturnState.get(), myContinuation.get());
}

template <typename F, typename... Args>
template <
	typename CF, typename CCallableType, typename... CArgs, typename CArgsTuple,
	typename CReturnType, typename CReturnState>
TypedTask<CF, CArgs...>& TypedTask<F, Args...>::then(CF&& f, CArgs&&... args)
{
	static_assert(sizeof(TypedTask<CF, CArgs...>) == sizeof(Task));

	myContinuation = std::make_unique<TypedTask<CF, CArgs...>>(
		std::forward<CCallableType>(f), std::forward<CArgs>(args)...);

	myInvokeFcnPtr = [](void* callablePtr, void* argsPtr, void* returnPtr, void* continuationPtr) {
		assert(continuationPtr);

		auto& callable = *static_cast<CCallableType*>(callablePtr);
		auto& args = *static_cast<CArgsTuple*>(argsPtr);
		auto& [value, flag] = *static_cast<CReturnState*>(returnPtr);
		auto& continuation = *static_cast<TypedTask<CF, CArgs...>*>(continuationPtr);

		if constexpr (std::is_void_v<ReturnType>)
		{
			std::apply(callable, args);
			flag.store(true, std::memory_order_release);
			flag.notify_all();
			continuation.invoke();
		}
		else
		{
			auto retval = std::apply(callable, args);
			flag.store(true, std::memory_order_release);
			flag.notify_all();
			continuation.invoke(std::move(retval));
		}
	};

	return *static_cast<TypedTask<CF, CArgs...>*>(myContinuation.get());
}

template <typename F, typename CallableType, typename... Args, typename ReturnType>
Future<ReturnType> ThreadPool::submit(TypedTask<F, Args...>&& task)
{
	ZoneScopedN("ThreadPool::submit");

	auto future = task.getFuture();

	{
		ZoneScopedN("ThreadPool::submit::enqueue");

		myQueue.enqueue(std::forward<TypedTask<F, Args...>>(task));
	}

	{
		ZoneScopedN("ThreadPool::submit::signal");

		//mySignal.notify_one();
		mySignal.release();
	}

	return future;
}

template <typename ReturnType>
std::optional<typename Future<ReturnType>::value_t>
ThreadPool::processQueueUntil(Future<ReturnType>&& future)
{
	ZoneScopedN("ThreadPool::processQueueUntil");

	return internalProcessQueue(std::forward<Future<ReturnType>>(future));
}

template <typename ReturnType>
std::optional<typename Future<ReturnType>::value_t>
ThreadPool::internalProcessQueue(Future<ReturnType>&& future)
{
	if (!future.valid())
		return std::nullopt;

	if (!future.is_ready())
	{
		Task task;
		while (myQueue.try_dequeue(task))
		{
			task.invoke();

			if (future.is_ready())
				break;
		}
	}

	return std::make_optional(future.get());
}
