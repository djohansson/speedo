#pragma once

#include "std_extra.h"
#include "upgradablesharedmutex.h"

#include <utility>

template <typename T, typename MutexT>
class ConcurrentAccess;

template <typename T, typename MutexT = UpgradableSharedMutex>
class ConcurrentReadScope
{
public:
	explicit ConcurrentReadScope() = delete;
	explicit ConcurrentReadScope(MutexT& mutex, const T& data) noexcept
		: myMutex(mutex)
		, myData(data)
	{
		myMutex.lock_shared();
	}
	explicit ConcurrentReadScope(const ConcurrentAccess<T, MutexT>& access) noexcept
		: ConcurrentReadScope(access.myMutex, access.myData) {}
	ConcurrentReadScope(ConcurrentReadScope&& other) noexcept = default;
	ConcurrentReadScope(const ConcurrentReadScope&) = delete;
	ConcurrentReadScope& operator=(const ConcurrentReadScope&) = delete;
	~ConcurrentReadScope() noexcept
	{
		myMutex.unlock_shared();
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

	const auto& Get() const noexcept { return myData; }

private:
	MutexT& myMutex;
	const T& myData;
};

template <typename T, typename MutexT = UpgradableSharedMutex>
class ConcurrentWriteScope
{
public:
	explicit ConcurrentWriteScope() noexcept = delete;
	explicit ConcurrentWriteScope(MutexT& mutex, T& data) noexcept
		: myMutex(mutex)
		, myData(data)
	{
		myMutex.lock();
	}
	explicit ConcurrentWriteScope(ConcurrentAccess<T, MutexT>& access) noexcept
		: ConcurrentWriteScope(access.myMutex, access.myData) {}
	ConcurrentWriteScope(ConcurrentWriteScope&& other) noexcept = default;
	ConcurrentWriteScope(const ConcurrentWriteScope&) = delete;
	ConcurrentWriteScope& operator=(const ConcurrentWriteScope&) = delete;
	~ConcurrentWriteScope() noexcept
	{
		myMutex.unlock();
	}

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
	MutexT& myMutex;
	T& myData;
};

template <typename T, typename MutexT = UpgradableSharedMutex>
class ConcurrentAccess
{
public:
	using value_t = T;
	using mutex_t = MutexT;

	ConcurrentAccess() noexcept = default;
	explicit ConcurrentAccess(T&& data) noexcept : myData(std::forward<T>(data)) {}
	ConcurrentAccess(ConcurrentAccess&& other) noexcept = default;
	ConcurrentAccess(const ConcurrentAccess&) = delete;
	ConcurrentAccess& operator=(ConcurrentAccess&& other) noexcept = default;
	ConcurrentAccess& operator=(const ConcurrentAccess&) = delete;	

	void Swap(ConcurrentAccess& rhs) noexcept
	{
		std::swap(myMutex, rhs.myMutex);
		std::swap(myData, rhs.myData);
	}
	friend void Swap(ConcurrentAccess& lhs, ConcurrentAccess& rhs) noexcept { lhs.Swap(rhs); }

private:
	friend class ConcurrentReadScope<T, MutexT>;
	friend class ConcurrentWriteScope<T, MutexT>;
	mutable MutexT myMutex{};
	T myData{};
};


