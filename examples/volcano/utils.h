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

// todo: c++20: std::atomic_ref
template <typename T>
class CopyableAtomic : public std::atomic<T>
{
public:
    
    CopyableAtomic() = default;
    constexpr CopyableAtomic(T val)
     : std::atomic<T>(val) 
    {
        this->store(val);
    }
    constexpr CopyableAtomic(const CopyableAtomic<T>& other)
    {
        this->store(other.load(std::memory_order_acquire), std::memory_order_release);
    }

    CopyableAtomic<T>& operator=(const CopyableAtomic<T>& other)
    {
        this->store(other.load(std::memory_order_acquire), std::memory_order_release);
        return *this;
    }
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
