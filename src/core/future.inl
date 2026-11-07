#include <core/assert.h>//NOLINT(modernize-deprecated-headers)

namespace core
{

template <typename T>
typename Future<T>::value_t Future<T>::Get()
{
	Wait();

	return myState->value;
}

template <typename T>
bool Future<T>::IsReady() const noexcept
{
	ENSUREF(Valid(), "Future is not valid!");

	return myState->latch.load(std::memory_order_acquire) == 0;
}

template <typename T>
void Future<T>::Wait() const
{
	ENSUREF(Valid(), "Future is not valid!");

	auto& latch = myState->latch;
	while (auto current = latch.load(std::memory_order_acquire))
		latch.wait(current, std::memory_order_acquire);
}

} // namespace core
