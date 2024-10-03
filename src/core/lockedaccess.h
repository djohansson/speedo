#pragma once

#include "std_extra.h"
#include "upgradablesharedmutex.h"

#include <utility>

template <typename T, typename MutexT>
class LockedReadScope
{
public:
	explicit LockedReadScope(const T& data, MutexT& mutex) noexcept
		: myMutex(mutex)
		, myData(data)
	{
		myMutex.lock_shared();
	}
	~LockedReadScope() noexcept { myMutex.unlock_shared(); }

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
	LockedReadScope() = delete;
	LockedReadScope(const LockedReadScope&) = delete;
	LockedReadScope(LockedReadScope&&) = delete;
	LockedReadScope& operator=(const LockedReadScope&) = delete;
	LockedReadScope& operator=(LockedReadScope&&) = delete;

	MutexT& myMutex;
	const T& myData;
};

template <typename T, typename MutexT>
class LockedWriteScope
{
public:
	explicit LockedWriteScope(T& data, MutexT& mutex) noexcept
		: myMutex(mutex)
		, myData(data)
	{
		myMutex.lock();
	}
	~LockedWriteScope() noexcept { myMutex.unlock(); }

	[[maybe_unused]] LockedWriteScope& operator=(T&& data) noexcept
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
	LockedWriteScope() = delete;
	LockedWriteScope(const LockedWriteScope&) = delete;
	LockedWriteScope(LockedWriteScope&&) = delete;
	LockedWriteScope& operator=(const LockedWriteScope&) = delete;
	LockedWriteScope& operator=(LockedWriteScope&&) = delete;

	MutexT& myMutex;
	T& myData;
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
		: myMutex(std::exchange(other.myMutex, {}))
		, myData(std::exchange(other.myData, {})) {}
	~LockedAccess() noexcept = default;

	[[maye_unused]] LockedAccess& operator=(LockedAccess&& other) noexcept
	{
		if (this != &other)
		{
			myMutex = std::exchange(other.myMutex, {});
			myData = std::exchange(other.myData, {});
		}
		return *this;
	}

	[[nodiscard]] auto Read() const noexcept { return LockedReadScope(myData, myMutex); }
	[[nodiscard]] auto Write() noexcept { return LockedWriteScope(myData, myMutex); }

	void Swap(LockedAccess& rhs) noexcept
	{
		std::swap(myMutex, rhs.myMutex);
		std::swap(myData, rhs.myData);
	}
	friend void Swap(LockedAccess& lhs, LockedAccess& rhs) noexcept { lhs.Swap(rhs); }

private:
	LockedAccess(const LockedAccess&) = delete;
	LockedAccess& operator=(const LockedAccess&) = delete;

	mutable MutexT myMutex{};
	T myData{};
};
