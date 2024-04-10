#pragma once

#include "copyableatomic.h"

#include <new>
#include <tuple>

class UpgradableSharedMutex
{
	using value_t = uint32_t;
	enum : value_t
	{
		Reader = 4,
		Upgraded = 2,
		Writer = 1,
		None = 0
	};
#if __cpp_lib_atomic_ref >= 201806
	static constexpr uint32_t Aligmnent = std::atomic_ref<value_t>::required_alignment;
	alignas(Aligmnent) value_t myBits = 0;
	std::atomic_ref<value_t> internalAtomicRef() noexcept { return std::atomic_ref(myBits); }
#else
#if __cpp_lib_hardware_interference_size >= 201603
	using std::hardware_constructive_interference_size;
	using std::hardware_destructive_interference_size;
#else
	static constexpr std::size_t hardware_constructive_interference_size = 64;
	static constexpr std::size_t hardware_destructive_interference_size = 64;
#endif
	static constexpr uint32_t Aligmnent = hardware_constructive_interference_size;
	alignas(Aligmnent) CopyableAtomic<value_t> myAtomic;
	CopyableAtomic<value_t>& internalAtomicRef() noexcept { return myAtomic; }
#endif

	template <typename Func>
	void internalAquireLock(Func lockFn) noexcept;

	template <value_t Expected = None>
	std::tuple<bool, value_t> internalTryLock() noexcept;
	std::tuple<bool, value_t> internalTryLockShared() noexcept;
	std::tuple<bool, value_t> internalTryLockUpgrade() noexcept;

public:
	// Lockable Concept
	void lock() noexcept;

	// Writer is responsible for clearing up both the Upgraded and Writer bits.
	void unlock() noexcept;

	// SharedLockable Concept
	void lock_shared() noexcept;
	void unlock_shared() noexcept;

	// Downgrade the lock from writer status to reader status.
	void unlock_and_lock_shared() noexcept;

	// UpgradeLockable Concept
	void lock_upgrade() noexcept;
	void unlock_upgrade() noexcept;

	// unlock upgrade and try to acquire write lock
	void unlock_upgrade_and_lock() noexcept;

	// unlock upgrade and read lock atomically
	void unlock_upgrade_and_lock_shared() noexcept;

	// write unlock and upgrade lock atomically
	void unlock_and_lock_upgrade() noexcept;

	// Attempt to acquire writer permission. Return false if we didn't get it.
	bool try_lock() noexcept;

	// Try to get reader permission on the lock. This can fail if we
	// find out someone is a writer or upgrader.
	// Setting the Upgraded bit would allow a writer-to-be to indicate
	// its intention to write and block any new readers while waiting
	// for existing readers to finish and release their read locks. This
	// helps avoid starving writers (promoted from upgraders).
	bool try_lock_shared() noexcept;

	// try to acquire an upgradable lock.
	bool try_lock_upgrade() noexcept;
};

#include "upgradablesharedmutex.inl"
