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

#include <ankerl/unordered_dense.h>

#include <concurrentqueue/concurrentqueue.h>

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
struct HandleHash : ankerl::unordered_dense::hash<Handle>
{
	size_t operator()(const T& obj) const
	{
		return ankerl::unordered_dense::hash<Handle>::operator()(static_cast<Handle>(obj));
	}
	size_t operator()(const Handle& handle) const
	{
		return ankerl::unordered_dense::hash<Handle>::operator()(handle);
	}
};

template <typename T, typename Handle>
struct HandleHash<std::shared_ptr<T>, Handle> : ankerl::unordered_dense::hash<Handle>
{
	size_t operator()(const std::shared_ptr<T>& ptr) const
	{
		return ankerl::unordered_dense::hash<Handle>::operator()(static_cast<Handle>(*ptr));
	}
	size_t operator()(const Handle& handle) const
	{
		return ankerl::unordered_dense::hash<Handle>::operator()(handle);
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
	typename KeyHash = ankerl::unordered_dense::hash<Key>,
	typename KeyEqualTo = std::equal_to<Key>>
using UnorderedMap = ankerl::unordered_dense::map<Key, Value, KeyHash, KeyEqualTo>;
template <
	typename Key,
	typename KeyHash = ankerl::unordered_dense::hash<Key>,
	typename KeyEqualTo = std::equal_to<Key>>
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

namespace std_extra
{

template <size_t N>
struct string_literal
{
	consteval string_literal(const char (&str)[N]) { std::copy_n(str, N, value); }

	char value[N];
};

template <string_literal S>
consteval std::string_view make_string_literal()
{
	return S.value;
}

template<typename... Tuples>
using tuple_cat_t = decltype(std::tuple_cat(std::declval<Tuples>()...));

template<typename, typename>
struct is_applicable : std::false_type {};

template<typename Func, template<typename...> typename Tuple, typename... Args>
struct is_applicable<Func, Tuple<Args...>> : std::is_invocable<Func, Args...> {};

template<typename F, typename Tuple>
concept applicable = is_applicable<F, Tuple>::value;

template <class F, class T, std ::size_t... I>
constexpr auto apply_impl(F&& f, T&& t, std::index_sequence<I...>) noexcept(
	std::is_nothrow_invocable<F&&, decltype(std::get<I>(std ::declval<T>()))...>{})
	-> std::invoke_result_t<F&&, decltype(std::get<I>(std ::declval<T>()))...>
{
	return invoke(std::forward<F>(f), std::get<I>(std::forward<T>(t))...);
}
template <typename F, typename Tuple>
using apply_result_t = decltype(apply_impl(
	std::declval<F>(),
	std::declval<Tuple>(),
	std::make_index_sequence<std ::tuple_size_v<std ::decay_t<Tuple>>>{}));

template <typename F, typename Tuple, typename = std::void_t<>>
class apply_result
{};
template <typename F, typename Tuple>
class apply_result<F, Tuple, std::void_t<apply_result_t<F, Tuple>>>
{
	using type = apply_result_t<F, Tuple>;
};

template <class F, class Tuple>
constexpr apply_result_t<F, Tuple> apply(F&& f, Tuple&& t) noexcept
{
	return apply_impl(std::forward<F>(f), std::forward<Tuple>(t), std::make_index_sequence<std::tuple_size_v<std::decay_t<Tuple>>>{});
}

}

