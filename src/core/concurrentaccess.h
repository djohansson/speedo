#pragma once

#include "std_extra.h"
#include "upgradablesharedmutex.h"

#include <utility>

template <typename T, typename MutexT = UpgradableSharedMutex>
class ConcurrentAccess
{
public:
	using value_t = T;
	using mutex_t = MutexT;

	ConcurrentAccess() noexcept {}
	ConcurrentAccess(T&& data) noexcept : myData(std::forward<T>(data)) {}
	ConcurrentAccess(ConcurrentAccess&& other) noexcept
		: myMutex(std::exchange(other.myMutex, {}))
		, myData(std::exchange(other.myData, {})) {}
	~ConcurrentAccess() noexcept = default;

	[[maye_unused]] ConcurrentAccess& operator=(ConcurrentAccess&& other) noexcept
	{
		if (this != &other)
		{
			myMutex = std::exchange(other.myMutex, {});
			myData = std::exchange(other.myData, {});
		}
		return *this;
	}

	[[nodiscard]] auto Read() const noexcept;
	[[nodiscard]] auto Write() noexcept;

	void Swap(ConcurrentAccess& rhs) noexcept
	{
		std::swap(myMutex, rhs.myMutex);
		std::swap(myData, rhs.myData);
	}
	friend void Swap(ConcurrentAccess& lhs, ConcurrentAccess& rhs) noexcept { lhs.Swap(rhs); }

private:
	ConcurrentAccess(const ConcurrentAccess&) = delete;
	ConcurrentAccess& operator=(const ConcurrentAccess&) = delete;

	mutable MutexT myMutex{};
	T myData{};
};

template <typename T, typename MutexT>
class ConcurrentReadScope
{
public:
	explicit ConcurrentReadScope(const T& data, MutexT& mutex) noexcept
		: myMutex(mutex)
		, myData(data)
	{
		myMutex.lock_shared();
	}
	ConcurrentReadScope(ConcurrentReadScope&&) = default;
	ConcurrentReadScope(ConcurrentAccess<T, MutexT>& access) noexcept
		: ConcurrentReadScope(access.Read()) {}
	~ConcurrentReadScope() noexcept { myMutex.unlock_shared(); }

	ConcurrentReadScope& operator=(ConcurrentReadScope&&) = default;

	[[nodiscard]] auto* operator->() const noexcept
	{
		if constexpr (std_extra::is_pointer_like_v<T>)
			return myData.operator->();
		else return &myData;
	}
	[[nodiscard]] auto& operator*() const noexcept
	{
		if constexpr (std_extra::is_pointer_like_v<T>)
			return myData.operator*();
		else return myData;
	}

	const auto& Get() const noexcept { return myData; }

private:
	ConcurrentReadScope() = delete;
	ConcurrentReadScope(const ConcurrentReadScope&) = delete;
	ConcurrentReadScope& operator=(const ConcurrentReadScope&) = delete;

	MutexT& myMutex;
	const T& myData;
};

template <typename T, typename MutexT>
class ConcurrentWriteScope
{
public:
	explicit ConcurrentWriteScope(T& data, MutexT& mutex) noexcept
		: myMutex(mutex)
		, myData(data)
	{
		myMutex.lock();
	}
	ConcurrentWriteScope(ConcurrentWriteScope&&) = default;
	ConcurrentWriteScope(ConcurrentAccess<T, MutexT>& access) noexcept
		: ConcurrentWriteScope(access.Write()) {}
	~ConcurrentWriteScope() noexcept { myMutex.unlock(); }

	ConcurrentWriteScope& operator=(ConcurrentWriteScope&&) = default;

	[[maybe_unused]] ConcurrentWriteScope& operator=(T&& data) noexcept
	{
		myData = std::forward<T>(data);
		return *this;
	}

	[[nodiscard]] auto* operator->() const noexcept
	{
		if constexpr (std_extra::is_pointer_like_v<T>)
			return myData.operator->();
		else return &myData;
	}
	[[nodiscard]] auto& operator*() const noexcept
	{
		if constexpr (std_extra::is_pointer_like_v<T>)
			return myData.operator*();
		else return myData;
	}

	auto& Get() noexcept { return myData; }
	
private:
	ConcurrentWriteScope() = delete;
	ConcurrentWriteScope(const ConcurrentWriteScope&) = delete;
	ConcurrentWriteScope& operator=(const ConcurrentWriteScope&) = delete;
	
	MutexT& myMutex;
	T& myData;
};

template <typename T, typename MutexT>
auto ConcurrentAccess<T, MutexT>::Read() const noexcept
{
	return ConcurrentReadScope(myData, myMutex);
}

template <typename T, typename MutexT>
auto ConcurrentAccess<T, MutexT>::Write() noexcept
{
	return ConcurrentWriteScope(myData, myMutex);
}
