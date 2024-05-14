#pragma once

#include "taskstate.h"

#include <memory>
#include <type_traits>

template <typename T>
class Future
{
public:
	using value_t = std::conditional_t<std::is_void_v<T>, std::nullptr_t, T>;

	constexpr Future() noexcept = default;
	Future(Future&& other) noexcept;
	Future(const Future& other) noexcept;
	
	Future& operator=(Future&& other) noexcept;
	Future& operator=(const Future& other) noexcept;

	value_t get();
	bool is_ready() const noexcept;
	bool valid() const noexcept;
	void Wait() const;

private:
	friend class Task;
	friend class TaskExecutor;

	struct FutureState : TaskState
	{
		value_t value;
	};

	Future(std::shared_ptr<FutureState>&& state) noexcept;

	std::shared_ptr<FutureState> myState;
};

#include "future.inl"
