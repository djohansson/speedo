#include "assert.h"//NOLINT(modernize-deprecated-headers)

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

template <typename T>
bool Future<T>::operator==(const Future& other) const noexcept
{
	return myState == other.myState;
}

template <typename T>
Future<T>& Future<T>::operator=(Future&& other) noexcept
{
	myState = std::exchange(other.myState, {});

	return *this;
}

template <typename T>
Future<T>& Future<T>::operator=(const Future& other) noexcept
{
	if (this == &other)
		return *this;

	myState = other.myState;

	return *this;
}

template <typename T>
typename Future<T>::value_t Future<T>::Get()
{
	Wait();

	// important copy! otherwise value will be garbage on exit due to myState.reset().
	auto retval = myState->value;

	myState.reset();

	return retval;
}

template <typename T>
bool Future<T>::IsReady() const noexcept
{
	ASSERTF(Valid(), "Future is not valid!");

	return myState->latch.load(std::memory_order_relaxed) == 0;
}

template <typename T>
bool Future<T>::Valid() const noexcept
{
	return !!myState;
}

template <typename T>
void Future<T>::Wait() const
{
	ASSERTF(Valid(), "Future is not valid!");

	while (auto current = myState->latch.load(std::memory_order_relaxed))
		myState->latch.wait(current, std::memory_order_acquire);
}
