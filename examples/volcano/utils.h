#pragma once

#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#if PROFILING_ENABLED
#ifndef TRACY_ENABLE
#define TRACY_ENABLE
#endif
#endif
#include <Tracy.hpp>

#include <cereal/cereal.hpp>
#include <cereal/types/string.hpp>

#if defined(__WINDOWS__)
#include <concurrent_unordered_map.h>
#include <concurrent_unordered_set.h>
#endif

#include <robin_hood.h>


#define clean_errno() (errno == 0 ? "None" : strerror(errno))
#define log_error(M, ...) fprintf(stderr, "[ERROR] (%s:%d: errno: %s) " M "\n", __FILE__, __LINE__, clean_errno(), ##__VA_ARGS__)
#define assertf(A, M, ...) if(!(A)) {log_error(M, ##__VA_ARGS__); assert(A); }

template <typename T>
inline constexpr auto sizeof_array(const T& array)
{
	return (sizeof(array) / sizeof(array[0]));
}

template <typename T>
bool is_ready(std::future<T> const& f)
{
	return f.wait_for(std::chrono::nanoseconds(0)) == std::future_status::ready;
}

inline uint32_t roundUp(uint32_t numToRound, uint32_t multiple)
{
	if (multiple == 0)
		return numToRound;

	auto remainder = numToRound % multiple;
	if (remainder == 0)
		return numToRound;

	return numToRound + multiple - remainder;
}

template <typename T>
class ArrayDeleter
{
	using DeleteFcn = std::function<void(T*, size_t)>;

public:

	ArrayDeleter() = default;
	ArrayDeleter(DeleteFcn&& deleter)
		: myDeleter(std::move(deleter))
	{}
	ArrayDeleter(DeleteFcn&& deleter, size_t size)
		: myDeleter(std::move(deleter))
		, mySize(size)
	{}

	inline void operator()(T* array) const
	{
		myDeleter(array, mySize);
	}

	inline size_t getSize() const { return mySize; }

private:

	DeleteFcn myDeleter = [](T* data, size_t){ delete [] data; };
	size_t mySize = 0;
};

class Noncopyable
{
public:
	Noncopyable() = default;
	~Noncopyable() = default;

private:
	Noncopyable(const Noncopyable&) = delete;
	Noncopyable& operator=(const Noncopyable&) = delete;
};

class Nondynamic
{
public:
	Nondynamic() = default;
	~Nondynamic() = default;

private:
	void *operator new(size_t);
    void *operator new[](size_t);
};

namespace std
{

template <class T0, class... Ts>
auto make_vector(T0&& first, Ts&&... args)
{
    using first_type = std::decay_t<T0>;
    return std::vector<first_type>{
        std::forward<T0>(first),
        std::forward<Ts>(args)...
    };
}

template <typename... Ts>
struct overloaded : Ts... { using Ts::operator()...; };

template <typename... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

template <typename>
inline constexpr bool always_false_v = false;

namespace filesystem
{

template <class Archive>
void CEREAL_LOAD_MINIMAL_FUNCTION_NAME(const Archive&, path& out, const std::string& in)
{
	out = in;
}

template <class Archive>
std::string CEREAL_SAVE_MINIMAL_FUNCTION_NAME(const Archive& ar, const path& p)
{
	return p.generic_string();
}

} // namespace filesystem

} // namespace std

CEREAL_SPECIALIZE_FOR_ALL_ARCHIVES(
	std::filesystem::path, cereal::specialization::non_member_load_save_minimal);

template <typename T, typename Handle>
struct HandleHash : robin_hood::hash<Handle>
{
	size_t operator()(const T& obj) const
	{
        return robin_hood::hash<Handle>::operator()(static_cast<Handle>(obj));
    }
	size_t operator()(const Handle& handle) const
	{
        return robin_hood::hash<Handle>::operator()(handle);
    }
};

template <typename T, typename Handle>
struct HandleHash<std::shared_ptr<T>, Handle> : robin_hood::hash<Handle>
{
	size_t operator()(const std::shared_ptr<T>& ptr) const
	{
        return robin_hood::hash<Handle>::operator()(static_cast<Handle>(*ptr));
    }
	size_t operator()(const Handle& handle) const
	{
        return robin_hood::hash<Handle>::operator()(handle);
    }

	using is_transparent = int;
};

template <typename T = void>
struct SharedPtrEqualTo : std::equal_to<T>
{
	constexpr bool operator()(const std::shared_ptr<T>& lhs, const std::shared_ptr<T>& rhs) const
	{
		return *lhs == *rhs;
	}
};

template <>
struct SharedPtrEqualTo<void> : std::equal_to<void>
{
	template <typename U, typename V>
	constexpr bool operator()(const std::shared_ptr<U>& lhs, const std::shared_ptr<V>& rhs) const
	{
		return *lhs == *rhs;
	}
	template <typename U, typename V>
	constexpr bool operator()(const std::shared_ptr<U>& lhs, const V& rhs) const
	{
		return *lhs == rhs;
	}
	template <typename U, typename V>
	constexpr bool operator()(const U& lhs, const std::shared_ptr<V>& rhs) const
	{
		return lhs == *rhs;
	}

	using is_transparent = int;
};

template <typename T>
struct PassThroughHash
{
	inline size_t operator()(const T& key) const { return static_cast<size_t>(key); }
};

template <typename Key, typename Value, typename KeyHash = robin_hood::hash<Key>, typename KeyEqualTo = std::equal_to<Key>>
using UnorderedMapType = robin_hood::unordered_map<Key, Value, KeyHash, KeyEqualTo>;
template <typename Key, typename KeyHash = robin_hood::hash<Key>, typename KeyEqualTo = std::equal_to<Key>>
using UnorderedSetType = robin_hood::unordered_set<Key, KeyHash, KeyEqualTo>;

#if defined(__WINDOWS__)
template <typename Key, typename Value, typename KeyHash = robin_hood::hash<Key>, typename KeyEqualTo = std::equal_to<Key>>
using ConcurrentUnorderedMapType = Concurrency::concurrent_unordered_map<Key, Value, KeyHash, KeyEqualTo>;
template <typename Key, typename KeyHash = robin_hood::hash<Key>, typename KeyEqualTo = std::equal_to<Key>>
using ConcurrentUnorderedSetType = Concurrency::concurrent_unordered_set<Key, KeyHash, KeyEqualTo>;
#else
template <typename Key, typename Value, typename KeyHash = robin_hood::hash<Key>, typename KeyEqualTo = std::equal_to<Key>>
using ConcurrentUnorderedMapType = UnorderedMapType<Key, Value, KeyHash, KeyEqualTo>;
template <typename Key, typename KeyHash = robin_hood::hash<Key>, typename KeyEqualTo = std::equal_to<Key>>
using ConcurrentUnorderedSetType = UnorderedSetType<Key, KeyHash, KeyEqualTo>;
#endif

class SpinMutex : public Noncopyable
{
	using value_t = uint8_t;
	enum : value_t { Reader = 4, Upgraded = 2, Writer = 1 };

public:

	constexpr SpinMutex() = default;
	SpinMutex(SpinMutex&& other) : myBits(std::exchange(other.myBits, 0)) {}
	~SpinMutex() { unlock(); }

	SpinMutex& operator=(SpinMutex&& other) { myBits = std::exchange(other.myBits, 0); return *this; }

	// Lockable Concept
	void lock()
	{
		uint_fast32_t count = 0;

		while (!try_lock())
		{
			_mm_pause();
			if (++count > 1000)
				std::this_thread::yield();
		}
	}

	// Writer is responsible for clearing up both the Upgraded and Writer bits.
	void unlock()
	{
		static_assert(Reader > Writer + Upgraded, "wrong bits!");

		auto bits = std::atomic_ref(myBits);
		
		bits.fetch_and(~(Writer | Upgraded), std::memory_order_release);
	}

	// SharedLockable Concept
	void lock_shared()
	{
		uint_fast32_t count = 0;

		while (!try_lock_shared())
		{
			_mm_pause();
			if (++count > 1000)
				std::this_thread::yield();
		}
	}

	void unlock_shared()
	{
		auto bits = std::atomic_ref(myBits);

		bits.fetch_add(-Reader, std::memory_order_release);
	}

	// Downgrade the lock from writer status to reader status.
	void unlock_and_lock_shared()
	{
		auto bits = std::atomic_ref(myBits);

		bits.fetch_add(Reader, std::memory_order_acquire);
		
		unlock();
	}

	// UpgradeLockable Concept
	void lock_upgrade()
	{
		uint_fast32_t count = 0;

		while (!try_lock_upgrade())
		{
			_mm_pause();
			if (++count > 1000)
				std::this_thread::yield();
		}
	}

	void unlock_upgrade()
	{
		auto bits = std::atomic_ref(myBits);

		bits.fetch_add(-Upgraded, std::memory_order_acq_rel);
	}

	// unlock upgrade and try to acquire write lock
	void unlock_upgrade_and_lock()
	{
		uint_fast32_t count = 0;

		while (!try_unlock_upgrade_and_lock())
		{
			_mm_pause();
			if (++count > 1000)
				std::this_thread::yield();
		}
	}

	// unlock upgrade and read lock atomically
	void unlock_upgrade_and_lock_shared()
	{
		auto bits = std::atomic_ref(myBits);

		bits.fetch_add(Reader - Upgraded, std::memory_order_acq_rel);
	}

	// write unlock and upgrade lock atomically
	void unlock_and_lock_upgrade()
	{
		// need to do it in two steps here -- as the Upgraded bit might be OR-ed at
		// the same time when other threads are trying do try_lock_upgrade().

		auto bits = std::atomic_ref(myBits);

		bits.fetch_or(Upgraded, std::memory_order_acquire);
		bits.fetch_add(-Writer, std::memory_order_release);
	}

	// Attempt to acquire writer permission. Return false if we didn't get it.
	bool try_lock()
	{
		auto bits = std::atomic_ref(myBits);
		
		value_t expect = 0;
		return bits.compare_exchange_strong(expect, Writer, std::memory_order_acq_rel);
	}

	// Try to get reader permission on the lock. This can fail if we
	// find out someone is a writer or upgrader.
	// Setting the Upgraded bit would allow a writer-to-be to indicate
	// its intention to write and block any new readers while waiting
	// for existing readers to finish and release their read locks. This
	// helps avoid starving writers (promoted from upgraders).
	bool try_lock_shared()
	{
		// fetch_add is considerably (100%) faster than compare_exchange,
		// so here we are optimizing for the common (lock success) case.

		auto bits = std::atomic_ref(myBits);
		
		value_t value = bits.fetch_add(Reader, std::memory_order_acquire);
		if (value & (Writer | Upgraded))
		{
			bits.fetch_add(-Reader, std::memory_order_release);
			return false;
		}
		return true;
	}

	// try to unlock upgrade and write lock atomically
	bool try_unlock_upgrade_and_lock()
	{
		auto bits = std::atomic_ref(myBits);

		value_t expect = Upgraded;
		return bits.compare_exchange_strong(expect, Writer, std::memory_order_acq_rel);
	}

	// try to acquire an upgradable lock.
	bool try_lock_upgrade()
	{
		auto bits = std::atomic_ref(myBits);
		
		value_t value = bits.fetch_or(Upgraded, std::memory_order_acquire);

		// Note: when failed, we cannot flip the Upgraded bit back,
		// as in this case there is either another upgrade lock or a write lock.
		// If it's a write lock, the bit will get cleared up when that lock's done
		// with unlock().
		return ((value & (Upgraded | Writer)) == 0);
	}

private:

	alignas(std::hardware_destructive_interference_size) value_t myBits = 0;
};
