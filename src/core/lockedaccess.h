#pragma once

#include "upgradablesharedmutex.h"

#include <utility>

template <typename T, typename MutexT>
class LockedReadScope
{
public:
	explicit LockedReadScope(const T& data, MutexT& mutex) noexcept
		: myData(data)
		, myMutex(mutex)
	{
		myMutex.lock_shared();
	}
	~LockedReadScope() noexcept { myMutex.unlock_shared(); }

	const T* operator->() const noexcept { return &myData; }
	const T& operator*() const noexcept { return myData; }

private:
	LockedReadScope() = delete;
	LockedReadScope(const LockedReadScope&) = delete;
	LockedReadScope(LockedReadScope&&) = delete;
	LockedReadScope& operator=(const LockedReadScope&) = delete;
	LockedReadScope& operator=(LockedReadScope&&) = delete;

	const T& myData;
	MutexT& myMutex;
};

template <typename T, typename MutexT>
class LockedWriteScope
{
public:
	explicit LockedWriteScope(T& data, MutexT& mutex) noexcept
		: myData(data)
		, myMutex(mutex)
	{
		myMutex.lock();
	}
	~LockedWriteScope() noexcept { myMutex.unlock(); }

	T* operator->() const noexcept { return &myData; }
	T& operator*() const noexcept { return myData; }

private:
	LockedWriteScope() = delete;
	LockedWriteScope(const LockedWriteScope&) = delete;
	LockedWriteScope(LockedWriteScope&&) = delete;
	LockedWriteScope& operator=(const LockedWriteScope&) = delete;
	LockedWriteScope& operator=(LockedWriteScope&&) = delete;

	T& myData;
	MutexT& myMutex;
};

template <typename T, typename MutexT = UpgradableSharedMutex>
class LockedAccess
{
public:
	using value_t = T;
	using mutex_t = MutexT;

	LockedAccess() noexcept {}
	LockedAccess(T&& data) noexcept : myData(std::forward<T>(data)) {}
	LockedAccess(LockedAccess&& other) noexcept
		: myData(std::exchange(other.myData, {}))
		, myMutex(std::exchange(other.myMutex, {})) {}
	~LockedAccess() noexcept = default;

	[[nodiscard]] LockedAccess& operator=(LockedAccess&& other) noexcept
	{
		if (this != &other)
		{
			myData = std::exchange(other.myData, {});
			myMutex = std::exchange(other.myMutex, {});
		}
		return *this;
	}

	[[nodiscard]] auto Read() noexcept { return LockedReadScope(myData, myMutex); }
	[[nodiscard]] auto Write() noexcept { return LockedWriteScope(myData, myMutex); }

	void Swap(LockedAccess& rhs) noexcept
	{
		std::swap(myData, rhs.myData);
		std::swap(myMutex, rhs.myMutex);
	}
	friend void Swap(LockedAccess& lhs, LockedAccess& rhs) noexcept { lhs.Swap(rhs); }

private:
	LockedAccess(const LockedAccess&) = delete;
	LockedAccess& operator=(const LockedAccess&) = delete;

	T myData{};
	MutexT myMutex{};
};
