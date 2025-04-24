#include "assert.h"//NOLINT(modernize-deprecated-headers)

template <typename T>
Future<T>::Future(std::shared_ptr<FutureState>&& state) noexcept
//	: myState(std::forward<std::shared_ptr<FutureState>>(state))
{
	std::atomic_store(&InternalState(), std::forward<std::shared_ptr<FutureState>>(state));
}

template <typename T>
Future<T>::Future(Future&& other) noexcept
//	: myState(std::exchange(other.InternalState(), {}))
{
	std::atomic_exchange(&InternalState(), std::atomic_load(&other.InternalState()));
}

template <typename T>
Future<T>::Future(const Future& other) noexcept
//	: myState(other.InternalState())
{
	std::atomic_store(&InternalState(), std::atomic_load(&other.InternalState()));
}

template <typename T>
bool Future<T>::operator==(const Future& other) const noexcept
{
	return std::atomic_load(&InternalState()) == std::atomic_load(&other.InternalState());
}

template <typename T>
Future<T>& Future<T>::operator=(Future&& other) noexcept
{
	std::atomic_exchange(&InternalState(), std::atomic_load(&other.InternalState()));

	return *this;
}

template <typename T>
Future<T>& Future<T>::operator=(const Future& other) noexcept
{
	if (this != &other)
		std::atomic_store(&InternalState(), std::atomic_load(&other.InternalState()));

	return *this;
}

template <typename T>
typename Future<T>::value_t Future<T>::Get()
{
	Wait();

	// important copy! otherwise value will be garbage on exit due to myState.reset().
	auto state = std::atomic_load(&InternalState());
	auto retval = state->value;

	state.reset();

	return retval;
}

template <typename T>
bool Future<T>::IsReady() const noexcept
{
	ASSERTF(Valid(), "Future is not valid!");

	auto& state = *std::atomic_load(&InternalState());

	return std::atomic_ref(state.latch).load(std::memory_order_relaxed) == 0;
}

template <typename T>
bool Future<T>::Valid() const noexcept
{
	return !!std::atomic_load(&InternalState());
}

template <typename T>
void Future<T>::Wait() const
{
	ASSERTF(Valid(), "Future is not valid!");

	auto& state = *std::atomic_load(&InternalState());

	auto latch = std::atomic_ref(state.latch);
	while (auto current = latch.load(std::memory_order_relaxed))
		latch.wait(current, std::memory_order_acquire);
}
