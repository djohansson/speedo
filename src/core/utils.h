#pragma once

#include "assert.h"//NOLINT(modernize-deprecated-headers)

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

#include <ankerl/unordered_dense.h>

#include <concurrentqueue/concurrentqueue.h>

template <typename T, typename Handle>
struct HandleHash : ankerl::unordered_dense::hash<Handle>
{
	[[nodiscard]] size_t operator()(const T& obj) const
	{
		return ankerl::unordered_dense::hash<Handle>::operator()(static_cast<Handle>(obj));
	}
	[[nodiscard]] size_t operator()(const Handle& handle) const
	{
		return ankerl::unordered_dense::hash<Handle>::operator()(handle);
	}

	using is_transparent = std::true_type;
};

template <typename T, typename Handle>
struct HandleHash<std::shared_ptr<T>, Handle> : ankerl::unordered_dense::hash<Handle>
{
	[[nodiscard]] size_t operator()(const std::shared_ptr<T>& ptr) const
	{
		return ankerl::unordered_dense::hash<Handle>::operator()(static_cast<Handle>(*ptr));
	}
	[[nodiscard]] size_t operator()(const Handle& handle) const
	{
		return ankerl::unordered_dense::hash<Handle>::operator()(handle);
	}

	using is_transparent = std::true_type;
};

template <typename T = void>
struct SharedPtrEqualTo : std::equal_to<T>
{
	[[nodiscard]] constexpr bool operator()(const std::shared_ptr<T>& lhs, const std::shared_ptr<T>& rhs) const
	{
		return *lhs == *rhs;
	}
};

template <>
struct SharedPtrEqualTo<void> : std::equal_to<void>
{
	template <typename U, typename V>
	[[nodiscard]] constexpr bool operator()(const std::shared_ptr<U>& lhs, const std::shared_ptr<V>& rhs) const
	{
		return *lhs == *rhs;
	}
	template <typename U, typename V>
	[[nodiscard]] constexpr bool operator()(const std::shared_ptr<U>& lhs, const V& rhs) const
	{
		return *lhs == rhs;
	}
	template <typename U, typename V>
	[[nodiscard]] constexpr bool operator()(const U& lhs, const std::shared_ptr<V>& rhs) const
	{
		return lhs == *rhs;
	}

	using is_transparent = int;
};

template <typename T>
struct IdentityHash
{
	[[nodiscard]] constexpr size_t operator()(const T& key) const noexcept { return static_cast<size_t>(key); }
};

class TupleHash
{
	template <typename T>
	struct Component
	{
		explicit Component(const T& value) : value(value) {}

		[[nodiscard]] uintmax_t operator,(uintmax_t n) const
		{
			n ^= std::hash<T>()(value);
			n ^= n << (sizeof(uintmax_t) * 4 - 1);
			return n ^ std::hash<uintmax_t>()(n);
		}

		const T& value;
	};

public:
	template <typename Tuple>
	[[nodiscard]] size_t operator()(const Tuple& tuple) const
	{
		return std::hash<uintmax_t>()(
			std::apply([](const auto&... xss) { return (Component(xss), ..., 0); }, tuple));
	}
};

template <
	typename Key,
	typename Value,
	typename KeyHash = ankerl::unordered_dense::hash<Key>,
	typename KeyEqualTo = std::equal_to<>>
using UnorderedMap = ankerl::unordered_dense::map<Key, Value, KeyHash, KeyEqualTo>;
template <
	typename Key,
	typename KeyHash = ankerl::unordered_dense::hash<Key>,
	typename KeyEqualTo = std::equal_to<>>
using UnorderedSet = ankerl::unordered_dense::set<Key, KeyHash, KeyEqualTo>;

template <typename T>
using ConcurrentQueue = moodycamel::ConcurrentQueue<T>;
using ProducerToken = moodycamel::ProducerToken;
using ConsumerToken = moodycamel::ConsumerToken;

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
	std::pair<iterator, bool> emplace(const Key& key, Args&&... args)//NOLINT(readability-identifier-naming)
	{
		auto elementIt = std::lower_bound(
			begin(),
			end(),
			key,
			[](const value_type& lhs, const key_type& rhs) { return lhs.first < rhs; });

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
	std::pair<iterator, bool> emplace(Args&&... args)//NOLINT(readability-identifier-naming)
	{
		auto key = Key(std::forward<Args>(args)...);
		auto elementIt = std::lower_bound(
			begin(), end(), key, [](const value_type& lhs, const value_type& rhs) { return lhs < rhs; });

		std::pair<iterator, bool> result(elementIt, false);
		if (elementIt == end() || key != *elementIt)
			result = std::make_pair(container_type::insert(elementIt, std::move(key)), true);

		return result;
	}

	[[nodiscard]] iterator find(const Key& key)//NOLINT(readability-identifier-naming)
	{
		auto elementIt = std::lower_bound(
			begin(), end(), key, [](const value_type& lhs, const Key& rhs) { return lhs < rhs; });

		return elementIt;
	}
};

template <typename T, typename ContainerT = FlatMap<T, T>>
class RangeSet : public ContainerT
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

	auto insert(value_type&& range)//NOLINT(readability-identifier-naming)
	{
		ASSERT(range.first < range.second);

		if constexpr (std::is_same_v<container_type, FlatMap<T, T>>)
		{
			auto currentCapacity = this->capacity();
			if (currentCapacity == this->size())
				this->reserve(currentCapacity + 1);
		}

		auto [low, high] = range;

		auto afterIt = std::upper_bound(
			begin(), end(), low, [](const T& lhs, const value_type& rhs) { return lhs < rhs.first; });
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
