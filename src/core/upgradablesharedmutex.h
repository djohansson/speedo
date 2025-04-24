#pragma once

#include <atomic>
#include <tuple>

//NOLINTBEGIN(readability-identifier-naming)

class UpgradableSharedMutex final
{
	using value_t = uint8_t;
	enum : value_t
	{
		Reader = 4,
		Upgraded = 2,
		Writer = 1,
		Empty = 0
	};
	static constexpr uint32_t kAligmnent = std::atomic_ref<value_t>::required_alignment;
	alignas(kAligmnent) value_t myBits = 0;
	[[nodiscard]] std::atomic_ref<value_t> InternalAtomicRef() noexcept { return std::atomic_ref(myBits); }

	template <typename Func>
	void InternalAquireLock(Func lockFn) noexcept;

	template <value_t Expected = Empty>
	[[nodiscard]] std::tuple<bool, value_t> InternalTryLock() noexcept;
	[[nodiscard]] std::tuple<bool, value_t> InternalTryLockShared() noexcept;
	[[nodiscard]] std::tuple<bool, value_t> InternalTryLockUpgrade() noexcept;

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
	[[nodiscard]] bool try_lock() noexcept;

	// Try to get reader permission on the lock. This can fail if we
	// find out someone is a writer or upgrader.
	// Setting the Upgraded bit would allow a writer-to-be to indicate
	// its intention to write and block any new readers while waiting
	// for existing readers to finish and release their read locks. This
	// helps avoid starving writers (promoted from upgraders).
	[[nodiscard]] bool try_lock_shared() noexcept;

	// try to acquire an upgradable lock.
	[[nodiscard]] bool try_lock_upgrade() noexcept;
};

#include "upgradablesharedmutex.inl"

//NOLINTEND(readability-identifier-naming)
