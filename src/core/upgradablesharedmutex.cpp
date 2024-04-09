#include "upgradablesharedmutex.h"

void UpgradableSharedMutex::lock() noexcept
{
	internalAquireLock([this]() { return internalTryLock(); });
}

void UpgradableSharedMutex::unlock() noexcept
{
	static_assert(Reader > Writer + Upgraded, "wrong bits!");

	internalAtomicRef().fetch_and(~(Writer | Upgraded), std::memory_order_release);
	internalAtomicRef().notify_all();
}

void UpgradableSharedMutex::lock_shared() noexcept
{
	internalAquireLock([this]() { return internalTryLockShared(); });
}

void UpgradableSharedMutex::unlock_shared() noexcept
{
	internalAtomicRef().fetch_add(-Reader, std::memory_order_release);
	internalAtomicRef().notify_all();
}

void UpgradableSharedMutex::unlock_and_lock_shared() noexcept
{
	internalAtomicRef().fetch_add(Reader, std::memory_order_acquire);

	unlock();
}

void UpgradableSharedMutex::lock_upgrade() noexcept
{
	internalAquireLock([this]() { return internalTryLockUpgrade(); });
}

void UpgradableSharedMutex::unlock_upgrade() noexcept
{
	internalAtomicRef().fetch_add(-Upgraded, std::memory_order_acq_rel);
	internalAtomicRef().notify_all();
}

void UpgradableSharedMutex::unlock_upgrade_and_lock() noexcept
{
	// try to unlock upgrade and write lock atomically
	internalAquireLock([this]() { return internalTryLock<Upgraded>(); });
}

void UpgradableSharedMutex::unlock_upgrade_and_lock_shared() noexcept
{
	internalAtomicRef().fetch_add(Reader - Upgraded, std::memory_order_acq_rel);
	internalAtomicRef().notify_all();
}

void UpgradableSharedMutex::unlock_and_lock_upgrade() noexcept
{
	// need to do it in two steps here -- as the Upgraded bit might be OR-ed at
	// the same time when other threads are trying do try_lock_upgrade().

	internalAtomicRef().fetch_or(Upgraded, std::memory_order_acquire);
	internalAtomicRef().fetch_add(-Writer, std::memory_order_release);
	internalAtomicRef().notify_all();
}

std::tuple<bool, typename UpgradableSharedMutex::value_t>
UpgradableSharedMutex::internalTryLockShared() noexcept
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

std::tuple<bool, typename UpgradableSharedMutex::value_t>
UpgradableSharedMutex::internalTryLockUpgrade() noexcept
{
	value_t value = internalAtomicRef().fetch_or(Upgraded, std::memory_order_acquire);

	// Note: when failed, we cannot flip the Upgraded bit back,
	// as in this case there is either another upgrade lock or a write lock.
	// If it's a write lock, the bit will get cleared up when that lock's done
	// with unlock().
	return std::make_tuple(((value & (Upgraded | Writer)) == 0), value);
}

bool UpgradableSharedMutex::try_lock() noexcept
{
	return std::get<0>(internalTryLock());
}

bool UpgradableSharedMutex::try_lock_shared() noexcept
{
	return std::get<0>(internalTryLockShared());
}

bool UpgradableSharedMutex::try_lock_upgrade() noexcept
{
	return std::get<0>(internalTryLockUpgrade());
}
