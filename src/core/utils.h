#pragma once

#include "assert.h"//NOLINT(modernize-deprecated-headers)
#include "std_extra.h"

#include <algorithm>
#include <flat_map>
#include <functional>
#include <memory>

#include <ankerl/unordered_dense.h>

#include <concurrentqueue/moodycamel/concurrentqueue.h>

template <std::size_t N>
struct MinSizeIndex
{
	static constexpr std_extra::min_unsigned_t<N> kInvalidIndex{static_cast<std_extra::min_unsigned_t<N>>(~0ull)};
	std_extra::min_unsigned_t<N> value{kInvalidIndex};

	[[nodiscard]] constexpr explicit operator bool() const noexcept { return value != kInvalidIndex; }
	[[nodiscard]] constexpr auto operator<=>(const MinSizeIndex&) const noexcept = default;
};

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
struct HandleHash<std::unique_ptr<T>, Handle> : ankerl::unordered_dense::hash<Handle>
{
	[[nodiscard]] size_t operator()(const std::unique_ptr<T>& ptr) const
	{
		return ankerl::unordered_dense::hash<Handle>::operator()(static_cast<Handle>(*ptr));
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

template <typename T, typename Handle>
struct HandleEqualTo : std::equal_to<T>
{
	[[nodiscard]] constexpr bool operator()(const T& lhs, const T& rhs) const
	{
		return static_cast<Handle>(lhs) == static_cast<Handle>(rhs);
	}

	[[nodiscard]] constexpr bool operator()(Handle lhs, const T& rhs) const
	{
		return lhs == static_cast<Handle>(rhs);
	}

	[[nodiscard]] constexpr bool operator()(const T& lhs, Handle rhs) const
	{
		return static_cast<Handle>(lhs) == rhs;
	}

	using is_transparent = std::true_type;
};

template <typename T, typename Handle>
struct HandleEqualTo<std::unique_ptr<T>, Handle> : std::equal_to<T>
{
	[[nodiscard]] constexpr bool operator()(const std::unique_ptr<T>& lhs, const std::unique_ptr<T>& rhs) const
	{
		return static_cast<Handle>(*lhs) == static_cast<Handle>(*rhs);
	}

	[[nodiscard]] constexpr bool operator()(Handle lhs, const std::unique_ptr<T>& rhs) const
	{
		return lhs == static_cast<Handle>(*rhs);
	}

	[[nodiscard]] constexpr bool operator()(const std::unique_ptr<T>& lhs, Handle rhs) const
	{
		return static_cast<Handle>(*lhs) == rhs;
	}

	using is_transparent = std::true_type;
};

template <typename T, typename Handle>
struct HandleEqualTo<std::shared_ptr<T>, Handle> : std::equal_to<T>
{
	[[nodiscard]] constexpr bool operator()(const std::shared_ptr<T>& lhs, const std::shared_ptr<T>& rhs) const
	{
		return static_cast<Handle>(*lhs) == static_cast<Handle>(*rhs);
	}

	[[nodiscard]] constexpr bool operator()(Handle lhs, const std::shared_ptr<T>& rhs) const
	{
		return lhs == static_cast<Handle>(*rhs);
	}

	[[nodiscard]] constexpr bool operator()(const std::shared_ptr<T>& lhs, Handle rhs) const
	{
		return static_cast<Handle>(*lhs) == rhs;
	}

	using is_transparent = std::true_type;
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

template <typename T, typename ContainerT = std::flat_map<T, T>>
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

		auto [low, high] = range;

		auto afterIt = std::upper_bound(
			begin(), end(), low, [](const T& lhs, const value_type& rhs) { return lhs < rhs.first; });
		auto isBegin = afterIt == begin();
		auto prevIt = isBegin ? afterIt : std::prev(afterIt);

		iterator insertRangeIt;

		if (isBegin || prevIt->second < low)
		{
			insertRangeIt = container_type::insert(afterIt, std::forward<value_type>(range));
			if constexpr (std::is_same_v<container_type, std::flat_map<T, T>>)
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

template<class... Ts>
struct Overloaded : Ts... { using Ts::operator()...; };
