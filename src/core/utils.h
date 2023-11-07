#pragma once

#include "assert.h"
#include "profiling.h"
#include "math.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <new>
#include <ranges>
#include <string>
#include <type_traits>
#include <vector>
#include <version>

#include <robin_hood.h>

#include <mio/mmap.hpp>



class Noncopyable
{
public:
	constexpr Noncopyable() = default;
	~Noncopyable() = default;

private:
	Noncopyable(const Noncopyable&) = delete;
	Noncopyable& operator=(const Noncopyable&) = delete;
};

class Nonmovable
{
public:
	constexpr Nonmovable() = default;
	~Nonmovable() = default;

private:
	Nonmovable(Nonmovable&&) = delete;
	Nonmovable& operator=(Nonmovable&&) = delete;
};

class Nondynamic
{
public:
	constexpr Nondynamic() = default;
	~Nondynamic() = default;

private:
	void* operator new(size_t);
	void* operator new[](size_t);
};

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

class TupleHash
{
	template <typename T>
	struct Component
	{
		Component(const T& value) : value(value) {}

		uintmax_t operator,(uintmax_t n) const
		{
			n ^= std::hash<T>()(value);
			n ^= n << (sizeof(uintmax_t) * 4 - 1);
			return n ^ std::hash<uintmax_t>()(n);
		}

		const T& value;
	};

public:
	template <typename Tuple>
	size_t operator()(const Tuple& tuple) const
	{
		return std::hash<uintmax_t>()(
			std::apply([](const auto&... xs) { return (Component(xs), ..., 0); }, tuple));
	}
};

template <
	typename Key,
	typename Value,
	typename KeyHash = robin_hood::hash<Key>,
	typename KeyEqualTo = std::equal_to<Key>>
using UnorderedMap = robin_hood::unordered_map<Key, Value, KeyHash, KeyEqualTo>;
template <
	typename Key,
	typename KeyHash = robin_hood::hash<Key>,
	typename KeyEqualTo = std::equal_to<Key>>
using UnorderedSet = robin_hood::unordered_set<Key, KeyHash, KeyEqualTo>;

template <typename Key, typename T, typename ContainerT = std::vector<std::pair<Key, T>>>
class FlatMap : public ContainerT
{
public:
	using container_type = ContainerT;
	using key_type = Key;
	using mapped_type = T;
	using value_type = typename container_type::value_type;
	using iterator = typename container_type::iterator;
	using container_type::begin;
	using container_type::empty;
	using container_type::end;

	template <typename... Args>
	std::pair<iterator, bool> emplace(const Key& key, Args&&... args)
	{
		auto elementIt = std::lower_bound(
			begin(),
			end(),
			key,
			[](const value_type& a, const key_type& b) { return a.first < b; });

		std::pair<iterator, bool> result(elementIt, false);
		if (elementIt == end() || key != elementIt->first)
			result = std::make_pair(
				container_type::emplace(elementIt, key, std::forward<Args>(args)...), true);

		return result;
	}
};

template <typename Key, typename ContainerT = std::vector<Key>>
class FlatSet : public ContainerT
{
public:
	using container_type = ContainerT;
	using key_type = Key;
	using value_type = typename container_type::value_type;
	using iterator = typename container_type::iterator;
	using container_type::begin;
	using container_type::empty;
	using container_type::end;

	template <typename... Args>
	std::pair<iterator, bool> emplace(Args&&... args)
	{
		auto key = Key(std::forward<Args>(args)...);
		auto elementIt = std::lower_bound(
			begin(), end(), key, [](const value_type& a, const value_type& b) { return a < b; });

		std::pair<iterator, bool> result(elementIt, false);
		if (elementIt == end() || key != *elementIt)
			result = std::make_pair(container_type::insert(elementIt, std::move(key)), true);

		return result;
	}
};

template <typename T, typename ContainerT = FlatMap<T, T>>
class RangeSet : private ContainerT
{
public:
	using container_type = ContainerT;
	using key_type = T;
	using mapped_type = T;
	using value_type = typename container_type::value_type;
	using iterator = typename container_type::iterator;
	using container_type::begin;
	using container_type::empty;
	using container_type::end;

	auto insert(value_type&& range)
	{
		assert(range.first < range.second);

		if constexpr (std::is_same_v<container_type, FlatMap<T, T>>)
		{
			auto currentCapacity = this->capacity();
			if (currentCapacity == this->size())
				this->reserve(currentCapacity + 1);
		}

		auto [low, high] = range;

		auto afterIt = std::upper_bound(
			begin(), end(), low, [](const T& a, const value_type& b) { return a < b.first; });
		auto isBegin = afterIt == begin();
		auto prevIt = isBegin ? afterIt : std::prev(afterIt);

		iterator insertRangeIt;

		if (isBegin || prevIt->second < low)
		{
			insertRangeIt = container_type::insert(afterIt, std::forward<value_type>(range));
			if constexpr (std::is_same_v<container_type, FlatMap<T, T>>)
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
			afterIt = container_type::erase(afterIt);
		}

		return std::make_pair(insertRangeIt, true);
	}
};

template <typename T, typename ContainerT = std::vector<T>, typename IndexT = uint32_t>
class CircularContainer : private ContainerT
{
public:
	using value_type = T;
	using container_type = ContainerT;
	using container_type::begin;
	using container_type::emplace_back;
	using container_type::end;

	T& get() { return container_type::at(myHead); }

	const T& get() const { return container_type::at(myHead); }

	T& fetchAdd(IndexT offset = 1u)
	{
		auto index = myHead;
		myHead = (myHead + offset) % container_type::size();
		return container_type::at(index);
	}

private:
	IndexT myHead{};
};

template <size_t N>
struct StringLiteral
{
	constexpr StringLiteral(const char (&str)[N]) { std::copy_n(str, N, value); }

	char value[N];
};

template <StringLiteral S>
constexpr const char* string_literal()
{
	return S.value;
}
