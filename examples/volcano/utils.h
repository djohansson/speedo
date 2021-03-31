#pragma once

#include <algorithm>
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
#include <new>
#include <string>
#include <type_traits>
#include <vector>
#include <version>

#include <cereal/cereal.hpp>
#include <cereal/types/string.hpp>

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

class Noncopyable
{
public:
	constexpr Noncopyable() = default;
	~Noncopyable() = default;

private:
	Noncopyable(const Noncopyable&) = delete;
	Noncopyable& operator=(const Noncopyable&) = delete;
};

class Nondynamic
{
public:
	constexpr Nondynamic() = default;
	~Nondynamic() = default;

private:
	void *operator new(size_t);
    void *operator new[](size_t);
};

template <typename T>
class ArrayDeleter : Noncopyable
{
	using DeleteFcn = std::function<void(T*, size_t)>;

public:

	constexpr ArrayDeleter() = default;
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

template <class T>
auto make_vector(size_t size)
{
    return std::vector<std::decay_t<T>>(size);
}

template <typename... Ts>
struct overloaded : Ts... { using Ts::operator()...; };

template <typename... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

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
struct IdentityHash
{
	constexpr size_t operator()(const T& key) const noexcept { return static_cast<size_t>(key); }
};

template <typename Key, typename Value, typename KeyHash = robin_hood::hash<Key>, typename KeyEqualTo = std::equal_to<Key>>
using UnorderedMap = robin_hood::unordered_map<Key, Value, KeyHash, KeyEqualTo>;
template <typename Key, typename KeyHash = robin_hood::hash<Key>, typename KeyEqualTo = std::equal_to<Key>>
using UnorderedSet = robin_hood::unordered_set<Key, KeyHash, KeyEqualTo>;

template <typename Key, typename Handle, typename KeyHash = HandleHash<Key, Handle>, typename KeyEqualTo = SharedPtrEqualTo<>>
using HandleSet = UnorderedSet<Key, KeyHash, KeyEqualTo>;

template <typename Key, typename T, typename VectorT = std::vector<std::pair<Key, T>>>
class FlatMap : public VectorT
{
public:

	using vector_type = VectorT;
	using key_type = Key;
	using mapped_type = T;
	using value_type = typename vector_type::value_type;
	using iterator = typename vector_type::iterator; 
	using vector_type::begin;
	using vector_type::end;
	using vector_type::emplace;
	using vector_type::insert;

	template <typename... Args>
	std::pair<iterator, bool> emplace(const Key& key, Args&&... args)
	{
		auto elementIt = std::lower_bound(begin(), end(), key, [](const value_type& a, const key_type& b){ return a.first < b; });

		std::pair<iterator, bool> result(elementIt, false);
		if (elementIt == end() || key != elementIt->first)
			result = std::make_pair(vector_type::emplace(elementIt, key, std::forward<Args>(args)...), true);

		return result;
	}

	std::pair<iterator, bool> insert(const value_type& value)
	{
		return std::apply(emplace, value);
	}
};

template <typename T, typename MapT = FlatMap<T, T>>
class RangeSet : public MapT
{
public:

	using map_type = MapT;
	using key_type = T;
	using mapped_type = T;
	using value_type = typename map_type::value_type;
	using iterator = typename map_type::iterator; 
	using map_type::begin;
	using map_type::end;
	using map_type::insert;
	using map_type::erase;

	auto insert(value_type&& range)
	{
		assert(range.first < range.second);

		if constexpr (std::is_same_v<map_type, FlatMap<T, T>>)
		{
			auto currentCapacity = this->capacity();
			if (currentCapacity == this->size())
				this->reserve(currentCapacity + 1);
		}

		auto [low, high] = range;

		auto afterIt = std::upper_bound(
			begin(),
			end(),
			low,
			[](const T& a, const value_type& b){ return a < b.first; });
		auto isBegin = afterIt == begin();
		auto prevIt = isBegin ? afterIt : std::prev(afterIt);
		
		iterator insertRangeIt;
		
		if (isBegin || prevIt->second < low)
		{
			insertRangeIt = insert(afterIt, std::move(range));
			if constexpr (std::is_same_v<map_type, FlatMap<T, T>>)
				afterIt = std::next(insertRangeIt); // since insert will have invalidated afterIt
		}
		else
		{
			insertRangeIt = prevIt;

			if (insertRangeIt->second >= high)
				return std::make_pair(insertRangeIt, false);
			else
				insertRangeIt->second = high;
		}

		while (afterIt != end() && high >= afterIt->first)
		{
			insertRangeIt->second = std::max(afterIt->second, insertRangeIt->second);
			afterIt = erase(afterIt);
		}

		return std::make_pair(insertRangeIt, true);
	}
};

template <typename T, typename ContainerT = std::vector<T>, typename IndexT = uint32_t>
class WrapContainer : public ContainerT
{
public:

	T& fetchAdd(IndexT offset = 1u)
	{
		auto index = myIndex;
		myIndex = (myIndex + offset) % ContainerT::size();
		return ContainerT::at(index);
	}

private:

	IndexT myIndex = {};
};
