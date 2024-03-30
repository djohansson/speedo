template <typename T>
Future<T>::Future(std::shared_ptr<FutureState>&& state) noexcept
	: myState(std::forward<std::shared_ptr<FutureState>>(state))
{}

template <typename T>
Future<T>::Future(Future&& other) noexcept : myState(std::exchange(other.myState, {}))
{}

template <typename T>
Future<T>::Future(const Future& other) noexcept
	: myState(other.myState)
{}

#include "utils.h"

template <typename T>
Future<T>& Future<T>::operator=(Future&& other) noexcept
{
	myState = std::exchange(other.myState, {});

	return *this;
}

template <typename T>
Future<T>& Future<T>::operator=(const Future& other) noexcept
{
	myState = other.myState;

	return *this;
}

template <typename T>
typename Future<T>::value_t Future<T>::get()
{
	wait();

	// important copy! otherwise value will be garbage on exit due to myState.reset().
	auto retval = myState->value;

	myState.reset();

	return retval;
}

template <typename T>
bool Future<T>::is_ready() const noexcept
{
	assertf(valid(), "Future is not valid!");

	return myState->latch.load(std::memory_order_relaxed) == 0;
}

template <typename T>
bool Future<T>::valid() const noexcept
{
	return !!myState;
}

template <typename T>
void Future<T>::wait() const
{
	assertf(valid(), "Future is not valid!");

	while (auto current = myState->latch.load(std::memory_order_relaxed))
		myState->latch.wait(current, std::memory_order_acquire);
}
