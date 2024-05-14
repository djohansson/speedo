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
	static constexpr uint32_t kAligmnent = std::atomic_ref<value_t>::required_alignment;
	alignas(kAligmnent) value_t myBits = 0;
	std::atomic_ref<value_t> InternalAtomicRef() noexcept { return std::atomic_ref(myBits); }
#else
#if __cpp_lib_hardware_interference_size >= 201603
	using std::hardware_constructive_interference_size;
	using std::hardware_destructive_interference_size;
#else
	static constexpr std::size_t hardware_constructive_interference_size = 64;
	static constexpr std::size_t hardware_destructive_interference_size = 64;
#endif
	static constexpr uint32_t kAligmnent = hardware_constructive_interference_size;
	alignas(kAligmnent) CopyableAtomic<value_t> myAtomic;
	CopyableAtomic<value_t>& InternalAtomicRef() noexcept { return myAtomic; }
#endif

	template <typename Func>
	void InternalAquireLock(Func lockFn) noexcept;

	template <value_t Expected = None>
	std::tuple<bool, value_t> InternalTryLock() noexcept;
	std::tuple<bool, value_t> InternalTryLockShared() noexcept;
	std::tuple<bool, value_t> InternalTryLockUpgrade() noexcept;

public:
	// Lockable Concept
	void lock() noexcept; // NOLINT(readability-identifier-naming.*)

	// Writer is responsible for clearing up both the Upgraded and Writer bits.
	void unlock() noexcept; // NOLINT(readability-identifier-naming.*)

	// SharedLockable Concept
	void lock_shared() noexcept; // NOLINT(readability-identifier-naming.*)
	void unlock_shared() noexcept; // NOLINT(readability-identifier-naming.*)

	// Downgrade the lock from writer status to reader status.
	void unlock_and_lock_shared() noexcept; // NOLINT(readability-identifier-naming.*)

	// UpgradeLockable Concept
	void lock_upgrade() noexcept; // NOLINT(readability-identifier-naming.*)
	void unlock_upgrade() noexcept; // NOLINT(readability-identifier-naming.*)

	// unlock upgrade and try to acquire write lock
	void unlock_upgrade_and_lock() noexcept; // NOLINT(readability-identifier-naming.*)

	// unlock upgrade and read lock atomically
	void unlock_upgrade_and_lock_shared() noexcept; // NOLINT(readability-identifier-naming.*)

	// write unlock and upgrade lock atomically
	void unlock_and_lock_upgrade() noexcept; // NOLINT(readability-identifier-naming.*)

	// Attempt to acquire writer permission. Return false if we didn't get it.
	bool try_lock() noexcept; // NOLINT(readability-identifier-naming.*)

	// Try to get reader permission on the lock. This can fail if we
	// find out someone is a writer or upgrader.
	// Setting the Upgraded bit would allow a writer-to-be to indicate
	// its intention to write and block any new readers while waiting
	// for existing readers to finish and release their read locks. This
	// helps avoid starving writers (promoted from upgraders).
	bool try_lock_shared() noexcept; // NOLINT(readability-identifier-naming.*)

	// try to acquire an upgradable lock.
	bool try_lock_upgrade() noexcept; // NOLINT(readability-identifier-naming.*)
};

#include "upgradablesharedmutex.inl"
