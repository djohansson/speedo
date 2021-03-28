#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <tuple>

#include "profiling.h"

template <typename T, typename AtomicT = std::atomic<T>>
class CopyableAtomic : public AtomicT
{
public:

	using AtomicT::store;
    
    constexpr CopyableAtomic() noexcept = default;
    constexpr CopyableAtomic(T val) noexcept : std::atomic<T>(val) {}
    CopyableAtomic(const CopyableAtomic<T>& other) noexcept
    {
        store(other.load(std::memory_order_acquire), std::memory_order_release);
    }
    CopyableAtomic<T>& operator=(const CopyableAtomic<T>& other) noexcept
    {
        store(other.load(std::memory_order_acquire), std::memory_order_release);

        return *this;
    }
};

template <typename T = uint32_t, uint32_t Aligmnent = 
#if __cpp_lib_hardware_interference_size >= 201603
std::hardware_destructive_interference_size>
#else
64>
#endif
class alignas(Aligmnent) UpgradableSharedMutex
#if __cpp_lib_atomic_ref < 201806
 : public CopyableAtomic<T>
#endif
{
	using value_t = T;
	enum : value_t { Reader = 4, Upgraded = 2, Writer = 1, None = 0 };

#if __cpp_lib_atomic_ref >= 201806
	value_t myBits = 0;
	inline auto internalAtomicRef() noexcept { return std::atomic_ref(myBits); }
#else
	inline auto& internalAtomicRef() noexcept { return *this; }
#endif

	template <typename Func>
	inline void internalSpinWait(Func lockFn) noexcept
	{
		auto result = lockFn();
		auto& [success, value] = result;
		while (!success)
		{
			ZoneScopedN("wait");

			internalAtomicRef().wait(value);
			result = lockFn();
		}
	}

public:

	// Lockable Concept
	void lock() noexcept
	{
		internalSpinWait([this](){ return try_lock(); });
	}

	// Writer is responsible for clearing up both the Upgraded and Writer bits.
	inline void unlock() noexcept
	{
		static_assert(Reader > Writer + Upgraded, "wrong bits!");

		internalAtomicRef().fetch_and(~(Writer | Upgraded), std::memory_order_release);
		internalAtomicRef().notify_all();
	}

	// SharedLockable Concept
	void lock_shared() noexcept
	{
		internalSpinWait([this](){ return try_lock_shared(); });
	}

	inline void unlock_shared() noexcept
	{
		internalAtomicRef().fetch_add(-Reader, std::memory_order_release);
		internalAtomicRef().notify_all();
	}

	// Downgrade the lock from writer status to reader status.
	inline void unlock_and_lock_shared() noexcept
	{
		internalAtomicRef().fetch_add(Reader, std::memory_order_acquire);
		
		unlock();
	}

	// UpgradeLockable Concept
	void lock_upgrade() noexcept
	{
		internalSpinWait([this](){ return try_lock_upgrade(); });
	}

	inline void unlock_upgrade() noexcept
	{
		internalAtomicRef().fetch_add(-Upgraded, std::memory_order_acq_rel);
		internalAtomicRef().notify_all();
	}

	// unlock upgrade and try to acquire write lock
	void unlock_upgrade_and_lock() noexcept
	{
		// try to unlock upgrade and write lock atomically
		internalSpinWait([this](){ return try_lock<Upgraded>(); });
	}

	// unlock upgrade and read lock atomically
	inline void unlock_upgrade_and_lock_shared() noexcept
	{
		internalAtomicRef().fetch_add(Reader - Upgraded, std::memory_order_acq_rel);
		internalAtomicRef().notify_all();
	}

	// write unlock and upgrade lock atomically
	inline void unlock_and_lock_upgrade() noexcept
	{
		// need to do it in two steps here -- as the Upgraded bit might be OR-ed at
		// the same time when other threads are trying do try_lock_upgrade().

		internalAtomicRef().fetch_or(Upgraded, std::memory_order_acquire);
		internalAtomicRef().fetch_add(-Writer, std::memory_order_release);
		internalAtomicRef().notify_all();
	}

	// Attempt to acquire writer permission. Return false if we didn't get it.
	template <value_t Expected = None>
	inline std::tuple<bool, value_t> try_lock() noexcept
	{
		auto result = std::make_tuple(false, Expected);
		auto& [success, value] = result;
		success = internalAtomicRef().compare_exchange_weak(value, Writer, std::memory_order_acq_rel);
		return result;
	}

	// Try to get reader permission on the lock. This can fail if we
	// find out someone is a writer or upgrader.
	// Setting the Upgraded bit would allow a writer-to-be to indicate
	// its intention to write and block any new readers while waiting
	// for existing readers to finish and release their read locks. This
	// helps avoid starving writers (promoted from upgraders).
	inline std::tuple<bool, value_t> try_lock_shared() noexcept
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

	// try to acquire an upgradable lock.
	inline std::tuple<bool, value_t> try_lock_upgrade() noexcept
	{
		value_t value = internalAtomicRef().fetch_or(Upgraded, std::memory_order_acquire);

		// Note: when failed, we cannot flip the Upgraded bit back,
		// as in this case there is either another upgrade lock or a write lock.
		// If it's a write lock, the bit will get cleared up when that lock's done
		// with unlock().
		return std::make_tuple(((value & (Upgraded | Writer)) == 0), value);
	}
};

template <typename T, typename DequeT = std::deque<T>>
class ConcurrentDeque : private DequeT
{
public:

	using deque_type = DequeT;
	using value_type = typename deque_type::value_type;

	void push_front(const value_type& src)
	{
		auto lock = std::unique_lock(myMutex);
		
		deque_type::push_front(src);
	}

	void emplace_front(value_type&& src)
	{
		auto lock = std::unique_lock(myMutex);

		deque_type::emplace_front(src);
	}

	void push_back(const value_type& src)
	{
		auto lock = std::unique_lock(myMutex);
		
		deque_type::push_back(src);
	}

	void emplace_back(value_type&& src)
	{
		auto lock = std::unique_lock(myMutex);

		deque_type::emplace_back(src);
	}

	bool try_pop_front(value_type& dst)
	{
		auto lock = std::unique_lock(myMutex);
		
		if (deque_type::empty())
			return false;
			
		dst = deque_type::front();
		deque_type::pop_front();

		return true;
	}

	bool try_pop_back(value_type& dst)
	{
		auto lock = std::unique_lock(myMutex);
		
		if (deque_type::empty())
			return false;
			
		dst = deque_type::back();
		deque_type::pop_back();

		return true;
	}

private:

	UpgradableSharedMutex<> myMutex;
};
