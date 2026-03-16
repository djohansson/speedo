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
	ConcurrentReadScope(const ConcurrentReadScope&) = delete;
	ConcurrentReadScope& operator=(const ConcurrentReadScope&) = delete;
	~ConcurrentReadScope() noexcept	{ myAccess.myMutex.unlock_shared(); }

	// todo: make accessors return const objects, requires some code restructuring
	[[nodiscard]] auto* operator->() const noexcept
	{
		if constexpr (std_extra::is_pointer_like_v<T>)
			return myAccess.myData.operator->();
		else return &myAccess.myData;
	}
	[[nodiscard]] auto& operator*() const noexcept
	{
		if constexpr (std_extra::is_pointer_like_v<T>)
			return myAccess.myData.operator*();
		else return myAccess.myData;
	}

	auto& Get() const noexcept { return myAccess.myData; }

private:
	friend class ConcurrentAccess<T, MutexT>;
	explicit ConcurrentReadScope(const ConcurrentAccess<T, MutexT>& access) noexcept
		: myAccess(access)
	{ myAccess.myMutex.lock_shared(); }
	ConcurrentReadScope(ConcurrentReadScope&& other) noexcept = default;

	const ConcurrentAccess<T, MutexT>& myAccess;
};

template <typename T, typename MutexT = UpgradableSharedMutex>
class ConcurrentWriteScope
{
public:
	explicit ConcurrentWriteScope() noexcept = delete;
	ConcurrentWriteScope(const ConcurrentWriteScope&) = delete;
	ConcurrentWriteScope& operator=(const ConcurrentWriteScope&) = delete;
	~ConcurrentWriteScope() noexcept { myAccess.myMutex.unlock(); }

	[[maybe_unused]] ConcurrentWriteScope& operator=(T&& data) noexcept
	{
		myAccess.myData = std::forward<T>(data);
		return *this;
	}

	[[nodiscard]] auto* operator->() const noexcept
	{
		if constexpr (std_extra::is_pointer_like_v<T>)
			return myAccess.myData.operator->();
		else return &myAccess.myData;
	}
	[[nodiscard]] auto& operator*() const noexcept
	{
		if constexpr (std_extra::is_pointer_like_v<T>)
			return myAccess.myData.operator*();
		else return myAccess.myData;
	}

	auto& Get() noexcept { return myAccess.myData; }
	
private:
	friend class ConcurrentAccess<T, MutexT>;
	explicit ConcurrentWriteScope(ConcurrentAccess<T, MutexT>& access) noexcept
		: myAccess(access)
	{ myAccess.myMutex.lock(); }
	ConcurrentWriteScope(ConcurrentWriteScope&& other) noexcept = default;

	ConcurrentAccess<T, MutexT>& myAccess;
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

	[[nodiscard]] ConcurrentReadScope<T, MutexT> Read() const noexcept { return ConcurrentReadScope<T, MutexT>(*this); }
	[[nodiscard]] ConcurrentWriteScope<T, MutexT> Write() noexcept { return ConcurrentWriteScope<T, MutexT>(*this); }

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


