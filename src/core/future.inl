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

	return myState->latch.load(std::memory_order_acquire) == 0;
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

	while (true)
	{
		auto current = myState->latch.load(std::memory_order_acquire);

		if (current == 0)
			return;

		myState->latch.wait(current, std::memory_order_acq_rel);
	}
}

// template <typename T>
// template <typename F, typename CallableType, typename... Args, typename ReturnType>
// requires std::invocable<F&, Args...> Future<ReturnType> Future<T>::then(F&& f)
// {
// 	auto& [value, flag, continuation] = *myState;

// 	continuation = Task(
// 		std::forward<CallableType>(f), std::make_shared<typename Future<ReturnType>::FutureState>());

// 	return Future<ReturnType>(
// 		continuation.template returnState<typename Future<ReturnType>::FutureState>());
// }
