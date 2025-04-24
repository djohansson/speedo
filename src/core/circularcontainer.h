#pragma once

#include <atomic>
#include <vector>

template <typename T, typename ContainerT = std::vector<T>>
class CircularContainer : public ContainerT
{
public:
	[[nodiscard]] T& Get() { return ContainerT::at(std::atomic_ref(myHead).load(std::memory_order_relaxed)); }
	[[nodiscard]] const T& Get() const { return ContainerT::at(std::atomic_ref(myHead).load(std::memory_order_relaxed)); }

	[[nodiscard]] T& FetchAdd(size_t offset = 1u)
	{
		auto head = std::atomic_ref(myHead);
		size_t index = head.fetch_add(offset, std::memory_order_relaxed);
		size_t newHeadWithOverflow = index + offset;
		size_t newHeadModulo = newHeadWithOverflow % ContainerT::size();
		while (!head.compare_exchange_weak(newHeadWithOverflow, newHeadModulo, std::memory_order_acq_rel))
			newHeadModulo = newHeadWithOverflow % ContainerT::size();
		
		return ContainerT::at(index);
	}

private:
	size_t myHead{};
};
