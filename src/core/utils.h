#pragma once

#include <core/assert.h>//NOLINT(modernize-deprecated-headers)
#include <core/std_extra.h>

#include <algorithm>
#include <cstdint>
#if __cpp_lib_flat_map >= 202207L
#include <flat_map>
#endif
#if __cpp_lib_flat_set >= 202207L
#include <flat_set>
#endif
#include <functional>
#include <memory>
#include <type_traits>

#if defined(USE_STD_UNORDERED_CONTAINERS) && USE_STD_UNORDERED_CONTAINERS
#include <unordered_map>
#include <unordered_set>
#else
#include <ankerl/unordered_dense.h>
#endif

#include <concurrentqueue/moodycamel/concurrentqueue.h>

template <typename T>
#if defined(USE_STD_UNORDERED_CONTAINERS) && USE_STD_UNORDERED_CONTAINERS
using Hash = std::hash<T>;
#else
using Hash = ankerl::unordered_dense::hash<T>;
#endif

template <std::size_t N>
struct MinSizeIndex
{
	static constexpr std_extra::min_unsigned_t<N> kInvalidIndex{static_cast<std_extra::min_unsigned_t<N>>(~0ull)};
	std_extra::min_unsigned_t<N> value{kInvalidIndex};

	[[nodiscard]] constexpr explicit operator bool() const noexcept { return value != kInvalidIndex; }
	[[nodiscard]] constexpr auto operator<=>(const MinSizeIndex&) const noexcept = default;
};

template <typename T, typename Handle>
struct HandleHash : Hash<Handle>
{
	[[nodiscard]] size_t operator()(const T& obj) const
	{
		return Hash<Handle>::operator()(static_cast<Handle>(obj));
	}
	[[nodiscard]] size_t operator()(const Handle& handle) const
	{
		return Hash<Handle>::operator()(handle);
	}

	using is_transparent = std::true_type;
};

template <typename T, typename Handle>
struct HandleHash<std::unique_ptr<T>, Handle> : Hash<Handle>
{
	[[nodiscard]] size_t operator()(const std::unique_ptr<T>& ptr) const
	{
		return Hash<Handle>::operator()(static_cast<Handle>(*ptr));
	}
	[[nodiscard]] size_t operator()(const Handle& handle) const
	{
		return Hash<Handle>::operator()(handle);
	}

	using is_transparent = std::true_type;
};

template <typename T, typename Handle>
struct HandleHash<std::shared_ptr<T>, Handle> : Hash<Handle>
{
	[[nodiscard]] size_t operator()(const std::shared_ptr<T>& ptr) const
	{
		return Hash<Handle>::operator()(static_cast<Handle>(*ptr));
	}
	[[nodiscard]] size_t operator()(const Handle& handle) const
	{
		return Hash<Handle>::operator()(handle);
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

template <typename T, typename TypeInfo, uint32_t Offset = 0>
struct IntrusiveTypeInfoHash : Hash<TypeInfo>
{
	[[nodiscard]] size_t operator()(const T& obj) const
	{
		const void* typePtr = static_cast<const void*>(reinterpret_cast<const std::byte*>(&obj) + Offset);
		const TypeInfo& type = *static_cast<const TypeInfo*>(typePtr);
		return Hash<TypeInfo>::operator()(type);
	}
	[[nodiscard]] size_t operator()(const TypeInfo& type) const
	{
		return Hash<TypeInfo>::operator()(type);
	}

	using is_transparent = std::true_type;
};

template <typename T, typename TypeInfo, uint32_t Offset = 0>
struct IntrusiveTypeInfoEqualTo : std::equal_to<T>
{
	[[nodiscard]] constexpr bool operator()(const T& lhs, const T& rhs) const
	{
		const void* lhsTypePtr = static_cast<const void*>(reinterpret_cast<const std::byte*>(&lhs) + Offset);
		const TypeInfo& lhsType = *static_cast<const TypeInfo*>(lhsTypePtr);
		const void* rhsTypePtr = static_cast<const void*>(reinterpret_cast<const std::byte*>(&rhs) + Offset);
		const TypeInfo& rhsType = *static_cast<const TypeInfo*>(rhsTypePtr);
		return lhsType == rhsType;
	}

	[[nodiscard]] constexpr bool operator()(TypeInfo lhs, const T& rhs) const
	{
		const void* rhsTypePtr = static_cast<const void*>(reinterpret_cast<const std::byte*>(&rhs) + Offset);
		const TypeInfo& rhsType = *static_cast<const TypeInfo*>(rhsTypePtr);
		return lhs == rhsType;
	}

	[[nodiscard]] constexpr bool operator()(const T& lhs, TypeInfo rhs) const
	{
		const void* lhsTypePtr = static_cast<const void*>(reinterpret_cast<const std::byte*>(&lhs) + Offset);
		const TypeInfo& lhsType = *static_cast<const TypeInfo*>(lhsTypePtr);
		return lhsType == rhs;
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
	typename KeyHash = Hash<Key>,
	typename KeyEqualTo = std::equal_to<>>
#if defined(USE_STD_UNORDERED_CONTAINERS) && USE_STD_UNORDERED_CONTAINERS
using UnorderedMap = std::unordered_map<Key, Value, KeyHash, KeyEqualTo>;
#else
using UnorderedMap = ankerl::unordered_dense::map<Key, Value, KeyHash, KeyEqualTo>;
#endif

template <
	typename Key,
	typename KeyHash = Hash<Key>,
	typename KeyEqualTo = std::equal_to<>>
#if defined(USE_STD_UNORDERED_CONTAINERS) && USE_STD_UNORDERED_CONTAINERS
using UnorderedSet = std::unordered_set<Key, KeyHash, KeyEqualTo>;
#else
using UnorderedSet = ankerl::unordered_dense::set<Key, KeyHash, KeyEqualTo>;
#endif

template <typename T>
using ConcurrentQueue = moodycamel::ConcurrentQueue<T>;
using ProducerToken = moodycamel::ProducerToken;
using ConsumerToken = moodycamel::ConsumerToken;

#if __cpp_lib_flat_map >= 202207L
template <typename T, typename ContainerT>
using FlatMap = std::flat_map<T, T>;
#else
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
#endif // #if __cpp_lib_flat_map >= 202207L
#if __cpp_lib_flat_set >= 202207L
using FlatSet = std::flat_set<T, T>;
#else
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
#endif // #if __cpp_lib_flat_set >= 202207L

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
		ENSURE(range.first < range.second);

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
