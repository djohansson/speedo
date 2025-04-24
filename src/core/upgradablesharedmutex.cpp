#include "upgradablesharedmutex.h"

void UpgradableSharedMutex::lock() noexcept
{
	InternalAquireLock([this]() { return InternalTryLock(); });
}

void UpgradableSharedMutex::unlock() noexcept
{
	static_assert(Reader > Writer + Upgraded, "wrong bits!");

	auto atomic = InternalAtomicRef();

	atomic.fetch_and(~(Writer | Upgraded), std::memory_order_release);
	atomic.notify_all();
}

void UpgradableSharedMutex::lock_shared() noexcept
{
	InternalAquireLock([this]() { return InternalTryLockShared(); });
}

void UpgradableSharedMutex::unlock_shared() noexcept
{
	auto atomic = InternalAtomicRef();

	atomic.fetch_add(-Reader, std::memory_order_release);
	atomic.notify_all();
}

void UpgradableSharedMutex::unlock_and_lock_shared() noexcept
{
	auto atomic = InternalAtomicRef();

	atomic.fetch_add(Reader, std::memory_order_acquire);

	unlock();
}

void UpgradableSharedMutex::lock_upgrade() noexcept
{
	InternalAquireLock([this]() { return InternalTryLockUpgrade(); });
}

void UpgradableSharedMutex::unlock_upgrade() noexcept
{
	auto atomic = InternalAtomicRef();

	atomic.fetch_add(-Upgraded, std::memory_order_acq_rel);
	atomic.notify_all();
}

void UpgradableSharedMutex::unlock_upgrade_and_lock() noexcept
{
	// try to unlock upgrade and write lock atomically
	InternalAquireLock([this]() { return InternalTryLock<Upgraded>(); });
}

void UpgradableSharedMutex::unlock_upgrade_and_lock_shared() noexcept
{
	auto atomic = InternalAtomicRef();

	atomic.fetch_add(Reader - Upgraded, std::memory_order_acq_rel);
	atomic.notify_all();
}

void UpgradableSharedMutex::unlock_and_lock_upgrade() noexcept
{
	auto atomic = InternalAtomicRef();

	// need to do it in two steps here -- as the Upgraded bit might be OR-ed at
	// the same time when other threads are trying do try_lock_upgrade().
	atomic.fetch_or(Upgraded, std::memory_order_acquire);
	atomic.fetch_add(-Writer, std::memory_order_release);
	atomic.notify_all();
}

std::tuple<bool, typename UpgradableSharedMutex::value_t>
UpgradableSharedMutex::InternalTryLockShared() noexcept
{
	auto atomic = InternalAtomicRef();

	// fetch_add is considerably (100%) faster than compare_exchange,
	// so here we are optimizing for the common (lock success) case.
	value_t value = atomic.fetch_add(Reader, std::memory_order_acquire);
	if ((value & (Writer | Upgraded)) != 0U)
	{
		value = atomic.fetch_add(-Reader, std::memory_order_release);
		return std::make_tuple(false, value);
	}
	return std::make_tuple(true, value);
}

std::tuple<bool, typename UpgradableSharedMutex::value_t>
UpgradableSharedMutex::InternalTryLockUpgrade() noexcept
{
	value_t value = InternalAtomicRef().fetch_or(Upgraded, std::memory_order_acquire);

	// Note: when failed, we cannot flip the Upgraded bit back,
	// as in this case there is either another upgrade lock or a write lock.
	// If it's a write lock, the bit will get cleared up when that lock's done
	// with unlock().
	return std::make_tuple(((value & (Upgraded | Writer)) == 0), value);
}

bool UpgradableSharedMutex::try_lock() noexcept
{
	return std::get<0>(InternalTryLock());
}

bool UpgradableSharedMutex::try_lock_shared() noexcept
{
	return std::get<0>(InternalTryLockShared());
}

bool UpgradableSharedMutex::try_lock_upgrade() noexcept
{
	return std::get<0>(InternalTryLockUpgrade());
}
