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
	internalAquireLock([this]() { return internalTryLock(); });
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
	internalAquireLock([this]() { return internalTryLockShared(); });
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
	internalAquireLock([this]() { return internalTryLockUpgrade(); });
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
	internalAquireLock([this]() { return internalTryLock<Upgraded>(); });
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
UpgradableSharedMutex<ValueT, Alignment>::internalTryLock() noexcept
{
	auto result = std::make_tuple(false, Expected);
	auto& [success, value] = result;
	success = internalAtomicRef().compare_exchange_weak(value, Writer, std::memory_order_acq_rel);
	return result;
}

template <typename ValueT, uint32_t Alignment>
std::tuple<bool, typename UpgradableSharedMutex<ValueT, Alignment>::value_t>
UpgradableSharedMutex<ValueT, Alignment>::internalTryLockShared() noexcept
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
UpgradableSharedMutex<ValueT, Alignment>::internalTryLockUpgrade() noexcept
{
	value_t value = internalAtomicRef().fetch_or(Upgraded, std::memory_order_acquire);

	// Note: when failed, we cannot flip the Upgraded bit back,
	// as in this case there is either another upgrade lock or a write lock.
	// If it's a write lock, the bit will get cleared up when that lock's done
	// with unlock().
	return std::make_tuple(((value & (Upgraded | Writer)) == 0), value);
}

template <typename ValueT, uint32_t Alignment>
bool UpgradableSharedMutex<ValueT, Alignment>::try_lock() noexcept
{
	return std::get<0>(internalTryLock());
}

template <typename ValueT, uint32_t Alignment>
bool UpgradableSharedMutex<ValueT, Alignment>::try_lock_shared() noexcept
{
	return std::get<0>(internalTryLockShared());
}

template <typename ValueT, uint32_t Alignment>
bool UpgradableSharedMutex<ValueT, Alignment>::try_lock_upgrade() noexcept
{
	return std::get<0>(internalTryLockUpgrade());
}
